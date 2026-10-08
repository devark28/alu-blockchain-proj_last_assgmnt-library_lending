#include "blockchain.h"
#include "merkle.h"
#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_TAG "B"
#define FIELDS_PER_BLOCK_LINE 14
#define HEADER_SIZE 640
#define PAYOUTS_TEXT_SIZE 200
#define FILE_LINE_SIZE 1100
#define PROBLEM_SIZE 160
#define ZERO_HASH "0000000000000000000000000000000000000000000000000000000000000000"

bool block_list_append(BlockList *list, const Block *block)
{
    if (list->count == list->capacity) {
        int new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        Block *grown = realloc(list->blocks, (size_t)new_capacity * sizeof(Block));
        if (grown == NULL) {
            printf("ERROR: out of memory while growing the chain\n");
            return false;
        }
        list->blocks = grown;
        list->capacity = new_capacity;
    }
    list->blocks[list->count++] = *block;
    return true;
}

void block_list_free(BlockList *list)
{
    free(list->blocks);
    list->blocks = NULL;
    list->count = 0;
    list->capacity = 0;
}

const Block *last_block(const BlockList *chain)
{
    return chain->count > 0 ? &chain->blocks[chain->count - 1] : NULL;
}

bool compute_block_merkle_root(const Block *block, char root[HASH_HEX_SIZE])
{
    char leaves[RECORDS_PER_BLOCK][HASH_HEX_SIZE];
    if (block->record_count < 1 || block->record_count > RECORDS_PER_BLOCK) {
        return false;
    }
    for (int i = 0; i < block->record_count; i++) {
        compute_record_leaf(&block->records[i], leaves[i]);
    }
    return compute_merkle_root(leaves, block->record_count, root);
}

/* Payouts are written as ID:amount pairs separated by commas. */
static void format_payouts(const Block *block, char buffer[PAYOUTS_TEXT_SIZE])
{
    buffer[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < block->payout_count; i++) {
        used += (size_t)snprintf(buffer + used, PAYOUTS_TEXT_SIZE - used, "%s%s:%d", i > 0 ? "," : "",
                                 block->payouts[i].account_id, block->payouts[i].amount);
    }
}

static bool parse_payouts(char *text, Block *block)
{
    block->payout_count = 0;
    if (text[0] == '\0') {
        return true;
    }

    char *pairs[MAX_PAYOUTS + 1];
    int pair_count = split_line(text, ',', pairs, MAX_PAYOUTS + 1);
    if (pair_count > MAX_PAYOUTS) {
        return false;
    }
    for (int i = 0; i < pair_count; i++) {
        char *parts[2];
        long long amount;
        if (split_line(pairs[i], ':', parts, 2) != 2
            || !copy_field(block->payouts[i].account_id, ID_SIZE, parts[0])
            || !parse_number(parts[1], 0, INT_MAX, &amount)) {
            return false;
        }
        block->payouts[i].amount = (int)amount;
    }
    block->payout_count = pair_count;
    return true;
}

/* Every header field except hash itself, joined with '|'. */
static void block_header(const Block *block, char *buffer, size_t size)
{
    char payouts[PAYOUTS_TEXT_SIZE];
    format_payouts(block, payouts);
    snprintf(buffer, size, "%d|%lld|%d|%s|%s|%s|%d|%s|%d|%lu|%d|%s", block->index, (long long)block->timestamp,
             block->record_count, block->previous_hash, block->merkle_root, block->sealed_by, block->token_reward,
             block->transaction_id, block->difficulty, block->nonce, block->block_reward, payouts);
}

void compute_block_hash(const Block *block, char output[HASH_HEX_SIZE])
{
    char header[HEADER_SIZE];
    block_header(block, header, sizeof(header));
    sha256_hex(header, output);
}

bool hash_meets_difficulty(const char *hash, int difficulty)
{
    for (int i = 0; i < difficulty; i++) {
        if (hash[i] != '0') {
            return false;
        }
    }
    return true;
}

/*
 * Fills in everything that depends on the block's place in the chain. The
 * timestamp is moved forward when needed, because several blocks can be
 * mined within one second and timestamps must strictly increase.
 */
void prepare_block(Block *block, const BlockList *chain, int difficulty, const char *sealed_by)
{
    const Block *previous = last_block(chain);
    block->index = chain->count;
    block->timestamp = time(NULL);
    if (previous != NULL && block->timestamp <= previous->timestamp) {
        block->timestamp = previous->timestamp + 1;
    }
    copy_field(block->previous_hash, sizeof(block->previous_hash), previous != NULL ? previous->hash : ZERO_HASH);
    copy_field(block->sealed_by, sizeof(block->sealed_by), sealed_by);
    block->difficulty = difficulty;
    block->nonce = 0;
    compute_block_merkle_root(block, block->merkle_root);
}

/*
 * Proof of work: try nonces until the hex hash starts with difficulty zeros.
 * Stops after max_attempts so pool and cloud miners can search in rounds;
 * the next call carries on from the nonce where this one stopped.
 */
bool search_nonce(Block *block, unsigned long max_attempts, unsigned long *attempts)
{
    for (unsigned long tried = 0; tried < max_attempts; tried++) {
        compute_block_hash(block, block->hash);
        (*attempts)++;
        if (hash_meets_difficulty(block->hash, block->difficulty)) {
            return true;
        }
        block->nonce++;
    }
    return false;
}

static bool repeats_a_record(const Block *block)
{
    for (int i = 0; i < block->record_count; i++) {
        for (int j = i + 1; j < block->record_count; j++) {
            if (strcmp(block->records[i].record_id, block->records[j].record_id) == 0) {
                return true;
            }
        }
    }
    return false;
}

/*
 * The very first record of the chain is the librarian registering their own
 * key. Nobody is registered yet, so it is checked against the key it carries;
 * that key is the trust anchor every later signature leads back to.
 */
static bool check_bootstrap_record(const Record *record, Keyring *identities, char problem[PROBLEM_SIZE])
{
    if (record->action != ACTION_REGISTERED || strcmp(record->member_id, LIBRARIAN_ID) != 0
        || strcmp(record->signer_id, LIBRARIAN_ID) != 0 || record->nonce != 0) {
        snprintf(problem, PROBLEM_SIZE, "the first record must be the librarian registering their key");
        return false;
    }
    Identity *librarian = add_identity(identities, LIBRARIAN_ID, record->public_key_hex);
    if (librarian == NULL) {
        snprintf(problem, PROBLEM_SIZE, "the librarian's public key is not a valid key");
        return false;
    }
    if (!record_signature_is_valid(record, librarian->public_key)) {
        snprintf(problem, PROBLEM_SIZE, "signature INVALID");
        return false;
    }
    librarian->next_nonce = 1;
    return true;
}

static void note_problem(char problem[PROBLEM_SIZE], const char *text)
{
    if (problem[0] == '\0') {
        copy_field(problem, PROBLEM_SIZE, text);
    }
}

/*
 * Checks one record against the identities registered before it: the ID
 * matches the contents, the right party signed it, the signature verifies
 * and the signer's nonce is exactly the next one expected.
 *
 * The signer's nonce and any registration are recorded even when the record
 * fails, so one tampered record is reported once instead of making every
 * later record from the same signer fail as well.
 */
static bool check_record(const Record *record, Keyring *identities, char problem[PROBLEM_SIZE])
{
    char text[PROBLEM_SIZE];
    char expected_id[HASH_HEX_SIZE];
    problem[0] = '\0';

    compute_record_id(record, expected_id);
    if (strcmp(expected_id, record->record_id) != 0) {
        note_problem(problem, "record_id does not match the record's contents");
    }
    if (!record_signer_is_authorized(record)) {
        snprintf(text, sizeof(text), "%s may not sign a %s record", record->signer_id, action_name(record->action));
        note_problem(problem, text);
    }

    Identity *signer = find_identity(identities, record->signer_id);
    if (signer == NULL) {
        snprintf(text, sizeof(text), "signer %s has no registered public key", record->signer_id);
        note_problem(problem, text);
        return false;
    }
    if (!record_signature_is_valid(record, signer->public_key)) {
        snprintf(text, sizeof(text), "signature INVALID for %s", record->signer_id);
        note_problem(problem, text);
    }
    if (record->nonce != signer->next_nonce) {
        snprintf(text, sizeof(text), "nonce %d from %s, expected %d", record->nonce, record->signer_id,
                 signer->next_nonce);
        note_problem(problem, text);
    }
    signer->next_nonce = record->nonce + 1;

    if (record->action == ACTION_REGISTERED) {
        if (find_identity(identities, record->member_id) != NULL) {
            snprintf(text, sizeof(text), "%s is already registered", record->member_id);
            note_problem(problem, text);
        } else if (add_identity(identities, record->member_id, record->public_key_hex) == NULL) {
            snprintf(text, sizeof(text), "public key for %s is not a valid key", record->member_id);
            note_problem(problem, text);
        }
    }
    return problem[0] == '\0';
}

/* Returns how many records failed, with the reason for each in problems. */
static int check_records(const Block *block, Keyring *identities, char problems[RECORDS_PER_BLOCK][PROBLEM_SIZE])
{
    int failures = 0;
    for (int i = 0; i < block->record_count; i++) {
        const Record *record = &block->records[i];
        bool is_bootstrap = block->index == 0 && i == 0;
        bool valid = is_bootstrap ? check_bootstrap_record(record, identities, problems[i])
                                  : check_record(record, identities, problems[i]);
        if (valid) {
            problems[i][0] = '\0';
        } else {
            failures++;
        }
    }
    return failures;
}

static const char *result(bool passed)
{
    return passed ? "OK  " : "FAIL";
}

/*
 * Walks the chain from the genesis block. Identities and nonces are rebuilt
 * from the chain itself as it goes, so no outside file is trusted. When
 * identities is not NULL it receives what was rebuilt, for the caller to use.
 */
bool validate_chain(const BlockList *chain, Keyring *identities, bool print_report)
{
    Keyring local_identities = {0};
    Keyring *rebuilt = identities != NULL ? identities : &local_identities;
    bool chain_valid = chain->count > 0;
    int first_bad_block = -1;

    for (int i = 0; i < chain->count; i++) {
        const Block *block = &chain->blocks[i];
        const Block *previous = i > 0 ? &chain->blocks[i - 1] : NULL;

        char recomputed_hash[HASH_HEX_SIZE];
        char recomputed_root[HASH_HEX_SIZE];
        compute_block_hash(block, recomputed_hash);
        bool root_computed = compute_block_merkle_root(block, recomputed_root);

        bool index_ok = block->index == i;
        bool time_ok = previous == NULL || block->timestamp > previous->timestamp;
        bool link_ok = strcmp(block->previous_hash, previous != NULL ? previous->hash : ZERO_HASH) == 0;
        bool hash_ok = strcmp(recomputed_hash, block->hash) == 0;
        bool work_ok = block->difficulty >= MIN_DIFFICULTY && block->difficulty <= MAX_DIFFICULTY
                       && hash_meets_difficulty(recomputed_hash, block->difficulty);
        bool merkle_ok = root_computed && strcmp(recomputed_root, block->merkle_root) == 0
                         && !repeats_a_record(block);

        char problems[RECORDS_PER_BLOCK][PROBLEM_SIZE];
        bool records_ok = root_computed && check_records(block, rebuilt, problems) == 0;

        if (print_report) {
            bool all_ok = index_ok && time_ok && link_ok && hash_ok && work_ok && merkle_ok && records_ok;
            printf("Block %-4d index %s time %s link %s hash %s pow %s merkle %s records %s%s\n", i,
                   result(index_ok), result(time_ok), result(link_ok), result(hash_ok), result(work_ok),
                   result(merkle_ok), result(records_ok), all_ok ? "" : " <-- TAMPERED");
            for (int r = 0; root_computed && r < block->record_count; r++) {
                if (problems[r][0] != '\0') {
                    printf("           record %d (%.12s...): %s\n", r, block->records[r].record_id, problems[r]);
                }
            }
        }

        bool block_valid = index_ok && time_ok && link_ok && hash_ok && work_ok && merkle_ok && records_ok;
        if (!block_valid && first_bad_block < 0) {
            first_bad_block = i;
        }
        chain_valid = chain_valid && block_valid;
    }

    if (print_report) {
        if (chain_valid) {
            printf("Chain is VALID: %d block%s checked.\n", chain->count, chain->count == 1 ? "" : "s");
        } else if (first_bad_block >= 0) {
            printf("Chain is INVALID: the first tampered block is %d.\n", first_bad_block);
        } else {
            printf("Chain is INVALID: it has no blocks.\n");
        }
    }
    if (rebuilt == &local_identities) {
        keyring_free(&local_identities);
    }
    return chain_valid;
}

static void format_block_line(const Block *block, char *buffer, size_t size)
{
    char payouts[PAYOUTS_TEXT_SIZE];
    format_payouts(block, payouts);
    snprintf(buffer, size, "%s|%d|%lld|%d|%s|%s|%s|%s|%d|%s|%d|%lu|%d|%s", BLOCK_TAG, block->index,
             (long long)block->timestamp, block->record_count, block->previous_hash, block->merkle_root,
             block->hash, block->sealed_by, block->token_reward, block->transaction_id, block->difficulty,
             block->nonce, block->block_reward, payouts);
}

static bool parse_block_line(char *line, Block *block)
{
    char *fields[FIELDS_PER_BLOCK_LINE];
    if (split_line(line, '|', fields, FIELDS_PER_BLOCK_LINE) != FIELDS_PER_BLOCK_LINE
        || strcmp(fields[0], BLOCK_TAG) != 0) {
        return false;
    }

    long long index, timestamp, record_count, token_reward, difficulty, nonce, block_reward;
    memset(block, 0, sizeof(*block));
    bool parsed = parse_number(fields[1], 0, INT_MAX, &index)
                  && parse_number(fields[2], 0, LLONG_MAX, &timestamp)
                  && parse_number(fields[3], 1, RECORDS_PER_BLOCK, &record_count)
                  && copy_field(block->previous_hash, sizeof(block->previous_hash), fields[4])
                  && copy_field(block->merkle_root, sizeof(block->merkle_root), fields[5])
                  && copy_field(block->hash, sizeof(block->hash), fields[6])
                  && copy_field(block->sealed_by, sizeof(block->sealed_by), fields[7])
                  && parse_number(fields[8], 0, INT_MAX, &token_reward)
                  && copy_field(block->transaction_id, sizeof(block->transaction_id), fields[9])
                  && parse_number(fields[10], 0, INT_MAX, &difficulty)
                  && parse_number(fields[11], 0, LLONG_MAX, &nonce)
                  && parse_number(fields[12], 0, INT_MAX, &block_reward)
                  && parse_payouts(fields[13], block);
    if (!parsed) {
        return false;
    }

    block->index = (int)index;
    block->timestamp = (time_t)timestamp;
    block->record_count = (int)record_count;
    block->token_reward = (int)token_reward;
    block->difficulty = (int)difficulty;
    block->nonce = (unsigned long)nonce;
    block->block_reward = (int)block_reward;
    return true;
}

/*
 * The whole chain is written to a temporary file which then replaces the
 * old one, so a crash part way through never leaves a half-written chain.
 */
bool save_chain(const char *path, const BlockList *chain)
{
    char temporary_path[256];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);

    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        printf("ERROR: cannot write %s (%s)\n", temporary_path, strerror(errno));
        return false;
    }

    char line[FILE_LINE_SIZE];
    for (int i = 0; i < chain->count; i++) {
        const Block *block = &chain->blocks[i];
        format_block_line(block, line, sizeof(line));
        fprintf(file, "%s\n", line);
        for (int r = 0; r < block->record_count; r++) {
            format_record_line(&block->records[r], line, sizeof(line));
            fprintf(file, "%s\n", line);
        }
    }

    if (fclose(file) != 0 || rename(temporary_path, path) != 0) {
        printf("ERROR: cannot save %s (%s)\n", path, strerror(errno));
        remove(temporary_path);
        return false;
    }
    return true;
}

static bool read_file_line(FILE *file, char line[FILE_LINE_SIZE], int *line_number)
{
    if (fgets(line, FILE_LINE_SIZE, file) == NULL) {
        return false;
    }
    (*line_number)++;
    line[strcspn(line, "\r\n")] = '\0';
    return true;
}

/*
 * Each B line is followed by exactly record_count R lines. A short block, a
 * stray R line or any field that does not parse stops the load with the
 * line number, rather than loading part of the chain.
 */
bool load_chain(const char *path, BlockList *chain)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        printf("ERROR: cannot open %s (%s)\n", path, strerror(errno));
        return false;
    }

    char line[FILE_LINE_SIZE];
    int line_number = 0;
    bool loaded = true;

    while (loaded && read_file_line(file, line, &line_number)) {
        Block block;
        if (!parse_block_line(line, &block)) {
            printf("ERROR: %s line %d is not a valid block line\n", path, line_number);
            loaded = false;
            break;
        }
        for (int r = 0; r < block.record_count; r++) {
            if (!read_file_line(file, line, &line_number) || !parse_record_line(line, &block.records[r])) {
                printf("ERROR: %s line %d should be record %d of block %d\n", path, line_number, r, block.index);
                loaded = false;
                break;
            }
        }
        loaded = loaded && block_list_append(chain, &block);
    }
    fclose(file);

    if (!loaded) {
        block_list_free(chain);
    }
    return loaded;
}

void print_block_summary(const Block *block)
{
    char created[TIME_TEXT_SIZE];
    format_time(block->timestamp, created);
    printf("  %-5d %s  %d record%s  sealed by %-10s difficulty %d  hash %.20s...\n", block->index, created,
           block->record_count, block->record_count == 1 ? " " : "s", block->sealed_by, block->difficulty,
           block->hash);
}

void print_block_details(const Block *block)
{
    char created[TIME_TEXT_SIZE];
    format_time(block->timestamp, created);

    printf("Block %d\n", block->index);
    printf("  timestamp:      %s\n", created);
    printf("  record_count:   %d\n", block->record_count);
    printf("  previous_hash:  %s\n", block->previous_hash);
    printf("  merkle_root:    %s\n", block->merkle_root);
    printf("  hash:           %s\n", block->hash);
    printf("  sealed_by:      %s\n", block->sealed_by);
    printf("  difficulty:     %d (nonce %lu)\n", block->difficulty, block->nonce);
    printf("  token_reward:   %d\n", block->token_reward);
    printf("  transaction_id: %s\n", block->transaction_id[0] != '\0' ? block->transaction_id : "-");
    printf("  block_reward:   %d", block->block_reward);
    for (int i = 0; i < block->payout_count; i++) {
        printf("%s %s %d", i == 0 ? ", paid to" : ",", block->payouts[i].account_id, block->payouts[i].amount);
    }
    printf("\n  records:\n");
    for (int i = 0; i < block->record_count; i++) {
        print_record(&block->records[i]);
    }

    char leaves[RECORDS_PER_BLOCK][HASH_HEX_SIZE];
    for (int i = 0; i < block->record_count; i++) {
        compute_record_leaf(&block->records[i], leaves[i]);
    }
    printf("  merkle tree:\n");
    print_merkle_tree(leaves, block->record_count);
}
