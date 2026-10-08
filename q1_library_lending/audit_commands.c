#include "commands.h"
#include "storage.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAMPER_EXTRA_DAYS 30

typedef bool (*RecordFilter)(const Record *record, const char *id);

static bool is_lending_record(const Record *record, const char *id)
{
    (void)id;
    return record->action != ACTION_REGISTERED && record->action != ACTION_TRANSFER
           && record->action != ACTION_FINE_PAID;
}

static bool involves_member(const Record *record, const char *member_id)
{
    return strcmp(record->member_id, member_id) == 0 || strcmp(record->signer_id, member_id) == 0
           || strcmp(record->recipient_id, member_id) == 0;
}

static bool involves_book(const Record *record, const char *book_id)
{
    return strcmp(record->book_id, book_id) == 0;
}

static void print_history_row(int block_index, const Record *record)
{
    char when[TIME_TEXT_SIZE];
    format_time(record->timestamp, when);
    printf("  %-5d %s  %-10s ", block_index, when, action_name(record->action));

    switch (record->action) {
    case ACTION_REGISTERED:
        printf("identity of %s registered by %s\n", record->member_id, record->signer_id);
        break;
    case ACTION_TRANSFER:
    case ACTION_FINE_PAID:
        printf("%s paid %d to %s\n", record->member_id, record->amount, record->recipient_id);
        break;
    case ACTION_RESERVED:
        printf("%s (%s) for %s\n", record->book_id, record->book_title, record->member_id);
        break;
    default:
        printf("%s (%s) %s", record->copy_id, record->book_title, record->member_id[0] != '\0' ? record->member_id : "-");
        if (record->fine_amount > 0) {
            printf(", fine %d", record->fine_amount);
        }
        if (record->condition[0] != '\0') {
            printf(", condition %s", record->condition);
        }
        if (record->reason_code[0] != '\0') {
            printf(", reason %s", record->reason_code);
        }
        printf("\n");
    }
}

/* Lists the confirmed records that match, oldest first, and counts matching requests still pending. */
static void print_history(const LibrarySystem *library, RecordFilter matches, const char *id, const char *title)
{
    int shown = 0;
    printf("%s\n", title);
    printf("  %-5s %-19s  %-10s %s\n", "block", "time", "action", "details");
    for (int b = 0; b < library->chain.count; b++) {
        const Block *block = &library->chain.blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            if (matches(&block->records[r], id)) {
                print_history_row(b, &block->records[r]);
                shown++;
            }
        }
    }
    if (shown == 0) {
        printf("  no confirmed records\n");
    }

    int pending = 0;
    for (int i = 0; i < library->pool.count; i++) {
        if (matches(&library->pool.items[i].record, id)) {
            pending++;
        }
    }
    if (pending > 0) {
        printf("  plus %d request%s still in the pending pool (pool_view)\n", pending, pending == 1 ? "" : "s");
    }
}

bool command_lending_history(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    print_history(library, is_lending_record, NULL, "Lending history, every confirmed lending record");
    return true;
}

bool command_member_history(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const Member *member = require_member(&library->state.registry, args[0]);
    if (member == NULL) {
        return false;
    }
    char title[96];
    snprintf(title, sizeof(title), "History of %s (%s)", member->member_id, member->full_name);
    print_history(library, involves_member, member->member_id, title);
    return true;
}

bool command_book_history(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const Book *book = require_book(&library->state.registry, args[0]);
    if (book == NULL) {
        return false;
    }
    char title[128];
    snprintf(title, sizeof(title), "History of %s (%s), all copies", book->book_id, book->title);
    print_history(library, involves_book, book->book_id, title);
    return true;
}

bool command_chain_save(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    if (!library_save(library)) {
        return false;
    }
    printf("Saved %s, %s, %s, %s, %s and %s (%d blocks).\n", CHAIN_FILE, POOL_FILE, INVENTORY_FILE,
           RESERVATIONS_FILE, LEDGER_FILE, META_FILE, library->chain.count);
    return true;
}

bool command_chain_load(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    printf("Reloading everything from disk and validating the chain again.\n");
    if (!library_reload(library)) {
        printf("ERROR: the files on disk could not be loaded; restart the program after fixing them\n");
        return false;
    }
    return library->chain_is_valid;
}

/* The newest block holding a BORROWED record, the kind of record whose due date could be quietly altered. */
static int find_tamper_target(const BlockList *chain, int *record_index)
{
    for (int b = chain->count - 1; b > 0; b--) {
        for (int r = 0; r < chain->blocks[b].record_count; r++) {
            if (chain->blocks[b].records[r].action == ACTION_BORROWED) {
                *record_index = r;
                return b;
            }
        }
    }
    return -1;
}

/*
 * Rebuilds what an attacker who can edit the file could rebuild: the record
 * ID, the Merkle root, and the proof of work for the edited block and every
 * block after it. Returns the hash attempts that took.
 */
static unsigned long cover_up(BlockList *chain, int target, int record_index)
{
    Block *edited = &chain->blocks[target];
    compute_record_id(&edited->records[record_index], edited->records[record_index].record_id);
    compute_block_merkle_root(edited, edited->merkle_root);

    unsigned long attempts = 0;
    for (int i = target; i < chain->count; i++) {
        Block *block = &chain->blocks[i];
        if (i > target) {
            memcpy(block->previous_hash, chain->blocks[i - 1].hash, HASH_HEX_SIZE);
        }
        block->nonce = 0;
        search_nonce(block, ULONG_MAX, &attempts);
    }
    return attempts;
}

/*
 * A two-step attack on a copy of the chain in memory; chain.dat is not
 * touched. Step 1 pushes a loan's due date back, the way someone might hide
 * a late return. Step 2 covers it up by recomputing every hash and redoing
 * the proof of work. Only the member's signature still gives it away.
 */
bool command_tamper_demo(LibrarySystem *library, int arg_count, char *args[])
{
    int record_index = 0;
    int target = find_tamper_target(&library->chain, &record_index);
    if (arg_count == 1) {
        long long index;
        if (!parse_number(args[0], 1, library->chain.count - 1, &index)) {
            printf("ERROR: block must be from 1 to %d\n", library->chain.count - 1);
            return false;
        }
        target = (int)index;
        record_index = 0;
    }
    if (target < 0) {
        printf("ERROR: there is no BORROWED record to tamper with yet. Borrow a book and mine it first.\n");
        return false;
    }

    BlockList copy = {0};
    for (int i = 0; i < library->chain.count; i++) {
        if (!block_list_append(&copy, &library->chain.blocks[i])) {
            block_list_free(&copy);
            return false;
        }
    }

    Record *record = &copy.blocks[target].records[record_index];
    char before[TIME_TEXT_SIZE];
    char after[TIME_TEXT_SIZE];
    format_time(record->due_date, before);
    record->due_date += (time_t)TAMPER_EXTRA_DAYS * DAY_SECONDS;
    format_time(record->due_date, after);

    printf("Tamper demo on a copy of the chain in memory; %s is not changed.\n\n", CHAIN_FILE);
    printf("Step 1: block %d, record %d (%s of %s by %s): due date moved from %s to %s\n", target, record_index,
           action_name(record->action), record->copy_id, record->member_id, before, after);
    validate_chain(&copy, NULL, true);

    printf("\nStep 2: the attacker recomputes the record ID, the Merkle root, and the proof of work for block %d "
           "and every block after it\n", target);
    unsigned long attempts = cover_up(&copy, target, record_index);
    printf("Re-mined %d block%s in %lu hash attempts.\n", copy.count - target, copy.count - target == 1 ? "" : "s",
           attempts);
    validate_chain(&copy, NULL, true);
    printf("\nEvery hash, link, proof of work and Merkle root checks out again, but the record's signature does "
           "not: rewriting it would need %s's private key.\n", record->signer_id);

    block_list_free(&copy);
    return true;
}
