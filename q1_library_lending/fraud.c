#include "fraud.h"
#include "inventory.h"

#include <stdio.h>
#include <string.h>

static void add_reason(char reason[FLAG_REASON_SIZE], const char *text)
{
    size_t used = strlen(reason);
    snprintf(reason + used, FLAG_REASON_SIZE - used, "%s%s", used > 0 ? "; " : "", text);
}

bool request_id_exists(const LibrarySystem *library, const char *record_id)
{
    for (int b = 0; b < library->chain.count; b++) {
        const Block *block = &library->chain.blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            if (strcmp(block->records[r].record_id, record_id) == 0) {
                return true;
            }
        }
    }
    for (int i = 0; i < library->pool.count; i++) {
        if (strcmp(library->pool.items[i].record.record_id, record_id) == 0) {
            return true;
        }
    }
    return false;
}

static int count_copies_held(const LibraryState *effective, const char *member_id)
{
    int held = 0;
    for (int i = 0; i < effective->registry.copy_count; i++) {
        const Copy *copy = &effective->registry.copies[i];
        if (copy->state == COPY_BORROWED && strcmp(copy->current_holder_id, member_id) == 0) {
            held++;
        }
    }
    return held;
}

static void update_latest_return(const Record *record, const char *member_id, const char *book_id, time_t *latest)
{
    if (record->action == ACTION_RETURNED && strcmp(record->member_id, member_id) == 0
        && strcmp(record->book_id, book_id) == 0 && record->timestamp > *latest) {
        *latest = record->timestamp;
    }
}

/* The latest time member_id returned a copy of book_id, on the chain or waiting in the pool; 0 if never. */
static time_t latest_return_time(const LibrarySystem *library, const char *member_id, const char *book_id)
{
    time_t latest = 0;
    for (int b = 0; b < library->chain.count; b++) {
        const Block *block = &library->chain.blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            update_latest_return(&block->records[r], member_id, book_id, &latest);
        }
    }
    for (int i = 0; i < library->pool.count; i++) {
        update_latest_return(&library->pool.items[i].record, member_id, book_id, &latest);
    }
    return latest;
}

/*
 * The misuse heuristics. Each one that matches adds to reason, and any
 * match makes the request SUSPICIOUS: it stays in the pool but no miner
 * takes it until a librarian approves it. effective is the state before
 * this request is applied.
 */
bool detect_misuse(const LibrarySystem *library, const LibraryState *effective, const Record *record,
                   char reason[FLAG_REASON_SIZE])
{
    char text[FLAG_REASON_SIZE];
    reason[0] = '\0';

    if (request_id_exists(library, record->record_id)) {
        add_reason(reason, "duplicate request: this request_id is already on the chain or in the pool");
    }

    if (record->action == ACTION_BORROWED) {
        int held = count_copies_held(effective, record->member_id);
        if (held + 1 > MAX_CONCURRENT_BORROWS) {
            snprintf(text, sizeof(text), "excessive borrowing: would hold %d copies, the limit is %d", held + 1,
                     MAX_CONCURRENT_BORROWS);
            add_reason(reason, text);
        }

        time_t returned_at = latest_return_time(library, record->member_id, record->book_id);
        if (returned_at > 0 && record->timestamp - returned_at < REBORROW_WINDOW_SECONDS) {
            add_reason(reason, "rapid re-borrow: same title borrowed again within a day of returning it");
        }
    }

    if (record->action == ACTION_RETURNED && record->reason_code[0] == '\0') {
        const Copy *copy = find_copy(&effective->registry, record->copy_id);
        CopyCondition returned_condition;
        if (copy != NULL && parse_condition(record->condition, &returned_condition)
            && condition_drop(copy->condition, returned_condition) > 1) {
            snprintf(text, sizeof(text), "condition manipulation: %s to %s without a reason code",
                     condition_name(copy->condition), record->condition);
            add_reason(reason, text);
        }
    }
    return reason[0] != '\0';
}
