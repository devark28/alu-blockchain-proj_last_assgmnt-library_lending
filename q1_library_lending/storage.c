#include "storage.h"
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define META_LINE_SIZE 160

/*
 * Each snapshot is rendered to text in memory first. The same text is
 * written to disk on save and compared with the file on startup, so the two
 * can never drift apart in format.
 */
typedef void (*SnapshotWriter)(FILE *out, const LibraryState *state);

static void write_inventory(FILE *out, const LibraryState *state)
{
    fprintf(out, "# copy_id|book_id|condition|state|holder|due_date|renewals|overdue_recorded|reserved_at\n");
    for (int i = 0; i < state->registry.copy_count; i++) {
        const Copy *copy = &state->registry.copies[i];
        fprintf(out, "%s|%s|%s|%s|%s|%lld|%d|%d|%lld\n", copy->copy_id, copy->book_id,
                condition_name(copy->condition), copy_state_name(copy->state), copy->current_holder_id,
                (long long)copy->due_date, copy->renewal_count, copy->overdue_recorded ? 1 : 0,
                (long long)copy->reserved_at);
    }
}

static void write_reservations(FILE *out, const LibraryState *state)
{
    fprintf(out, "# book_id|position|member_id|reservation_date\n");
    for (int i = 0; i < state->reservations.count; i++) {
        const ReservationQueue *queue = &state->reservations.queues[i];
        int position = 1;
        for (const ReservationNode *node = queue->head; node != NULL; node = node->next) {
            fprintf(out, "%s|%d|%s|%lld\n", queue->book_id, position++, node->member_id,
                    (long long)node->reservation_date);
        }
    }
}

/*
 * The ledger snapshot holds the active model's balances (U lines for the
 * UTXO set, A lines for accounts), every identity's next nonce (N) and
 * every unpaid fine (F).
 */
static void write_ledger(FILE *out, const LibraryState *state)
{
    const Ledger *ledger = &state->ledger;
    fprintf(out, "# model %s\n", model_name(ledger->model));
    if (ledger->model == MODEL_UTXO) {
        for (int i = 0; i < ledger->utxos.count; i++) {
            const Utxo *utxo = &ledger->utxos.items[i];
            fprintf(out, "U|%s|%d|%s|%d\n", utxo->txid, utxo->output_index, utxo->owner, utxo->amount);
        }
    } else {
        for (int i = 0; i < ledger->accounts.count; i++) {
            const Account *account = &ledger->accounts.items[i];
            fprintf(out, "A|%s|%d\n", account->id, account->balance);
        }
    }
    for (int i = 0; i < state->identities.count; i++) {
        fprintf(out, "N|%s|%d\n", state->identities.items[i].id, state->identities.items[i].next_nonce);
    }
    for (int i = 0; i < state->registry.member_count; i++) {
        const Member *member = &state->registry.members[i];
        if (member->fines_owed > 0) {
            fprintf(out, "F|%s|%d\n", member->member_id, member->fines_owed);
        }
    }
}

static char *render(SnapshotWriter writer, const LibraryState *state)
{
    char *text = NULL;
    size_t length = 0;
    FILE *out = open_memstream(&text, &length);
    if (out == NULL) {
        return NULL;
    }
    writer(out, state);
    fclose(out);
    return text;
}

static char *read_whole_file(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }
    char *text = NULL;
    size_t length = 0;
    FILE *out = open_memstream(&text, &length);
    if (out == NULL) {
        fclose(file);
        return NULL;
    }
    int character;
    while ((character = fgetc(file)) != EOF) {
        fputc(character, out);
    }
    fclose(out);
    fclose(file);
    return text;
}

/* Writes text to a temporary file and renames it over path, so a crash never leaves half a file. */
static bool write_atomically(const char *path, const char *text)
{
    char temporary_path[256];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);
    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        printf("ERROR: cannot write %s (%s)\n", temporary_path, strerror(errno));
        return false;
    }
    fputs(text, file);
    if (fclose(file) != 0 || rename(temporary_path, path) != 0) {
        printf("ERROR: cannot save %s (%s)\n", path, strerror(errno));
        remove(temporary_path);
        return false;
    }
    return true;
}

static bool save_snapshot(const char *path, SnapshotWriter writer, const LibraryState *state)
{
    char *text = render(writer, state);
    bool saved = text != NULL && write_atomically(path, text);
    free(text);
    return saved;
}

bool save_snapshots(const LibraryState *state, const BlockList *chain, int difficulty)
{
    char meta[META_LINE_SIZE * 6];
    const Block *tip = last_block(chain);
    snprintf(meta, sizeof(meta), "difficulty=%d\nblock_reward=%d\nmodel=%s\nheight=%d\ntip_hash=%s\n", difficulty,
             BLOCK_REWARD, state->ledger.model == MODEL_UTXO ? "utxo" : "account", chain->count,
             tip != NULL ? tip->hash : "");

    return save_snapshot(INVENTORY_FILE, write_inventory, state)
           && save_snapshot(RESERVATIONS_FILE, write_reservations, state)
           && save_snapshot(LEDGER_FILE, write_ledger, state)
           && write_atomically(META_FILE, meta);
}

static int first_different_line(const char *expected, const char *actual)
{
    int line = 1;
    while (*expected != '\0' && *expected == *actual) {
        if (*expected == '\n') {
            line++;
        }
        expected++;
        actual++;
    }
    return line;
}

/*
 * The state rebuilt from the chain is what the program uses. A snapshot
 * that differs from it was edited (or saved by a different version), so it
 * is reported here and overwritten at the next save.
 */
static void compare_snapshot(const char *path, SnapshotWriter writer, const LibraryState *state)
{
    char *saved = read_whole_file(path);
    char *rebuilt = render(writer, state);
    if (saved == NULL) {
        printf("Note: %s not found, it will be created from the chain.\n", path);
    } else if (rebuilt != NULL && strcmp(saved, rebuilt) != 0) {
        printf("WARNING: %s does not match the chain from line %d. It was changed outside the program; the state "
               "rebuilt from the chain is used and the file will be rewritten.\n",
               path, first_different_line(rebuilt, saved));
    }
    free(saved);
    free(rebuilt);
}

void compare_snapshots(const LibraryState *state)
{
    compare_snapshot(INVENTORY_FILE, write_inventory, state);
    compare_snapshot(RESERVATIONS_FILE, write_reservations, state);

    char *saved = read_whole_file(LEDGER_FILE);
    char expected_header[32];
    snprintf(expected_header, sizeof(expected_header), "# model %s\n", model_name(state->ledger.model));
    bool same_model = saved == NULL || strncmp(saved, expected_header, strlen(expected_header)) == 0;
    free(saved);
    if (same_model) {
        compare_snapshot(LEDGER_FILE, write_ledger, state);
    } else {
        printf("Note: %s was saved under the other ledger model, so it is rebuilt for this one.\n", LEDGER_FILE);
    }
}

static bool parse_meta_line(char *line, ChainMetadata *metadata)
{
    char *equals = strchr(line, '=');
    if (equals == NULL) {
        return false;
    }
    *equals = '\0';
    const char *key = line;
    const char *value = equals + 1;
    long long number;

    if (strcmp(key, "difficulty") == 0 && parse_number(value, MIN_DIFFICULTY, MAX_DIFFICULTY, &number)) {
        metadata->difficulty = (int)number;
    } else if (strcmp(key, "block_reward") == 0 && parse_number(value, 0, INT_MAX, &number)) {
        metadata->block_reward = (int)number;
    } else if (strcmp(key, "model") == 0) {
        return parse_model(value, &metadata->model);
    } else if (strcmp(key, "height") == 0 && parse_number(value, 0, INT_MAX, &number)) {
        metadata->height = (int)number;
    } else if (strcmp(key, "tip_hash") == 0) {
        return copy_field(metadata->tip_hash, HASH_HEX_SIZE, value);
    } else {
        return false;
    }
    return true;
}

bool load_metadata(ChainMetadata *metadata)
{
    memset(metadata, 0, sizeof(*metadata));
    metadata->difficulty = DEFAULT_DIFFICULTY;
    metadata->model = MODEL_UTXO;

    FILE *file = fopen(META_FILE, "r");
    if (file == NULL) {
        return true;
    }
    char line[META_LINE_SIZE];
    int line_number = 0;
    bool loaded = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0' && !parse_meta_line(line, metadata)) {
            printf("ERROR: %s line %d is not valid\n", META_FILE, line_number);
            loaded = false;
            break;
        }
    }
    fclose(file);
    metadata->found = loaded;
    return loaded;
}

/*
 * Deleting blocks from the end leaves a shorter chain in which every hash,
 * link and signature still checks out. meta.dat remembers how long the chain
 * was and the hash of its last block, which exposes that.
 */
bool check_chain_against_metadata(const BlockList *chain, const ChainMetadata *metadata)
{
    if (!metadata->found || metadata->height == 0) {
        return true;
    }
    if (chain->count < metadata->height) {
        printf("WARNING: %s was saved with %d blocks but chain.dat now has %d: blocks were removed from the end, so the chain is INVALID.\n",
               META_FILE, metadata->height, chain->count);
        return false;
    }
    if (strcmp(chain->blocks[metadata->height - 1].hash, metadata->tip_hash) != 0) {
        printf("WARNING: block %d is not the block %s recorded as the tip: the chain was replaced, so it is INVALID.\n",
               metadata->height - 1, META_FILE);
        return false;
    }
    return true;
}
