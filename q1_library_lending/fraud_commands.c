#include "commands.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

static void print_review_entry(const PendingRequest *request)
{
    const Record *record = &request->record;
    char submitted[TIME_TEXT_SIZE];
    format_time(record->timestamp, submitted);
    printf("  %.12s  %-10s signed by %-9s nonce %-3d %s\n", record->record_id, action_name(record->action),
           record->signer_id, record->nonce, submitted);
    if (record->copy_id[0] != '\0') {
        printf("      %s (%s) for %s", record->copy_id, record->book_title, record->member_id);
        if (record->condition[0] != '\0') {
            printf(", returned %s", record->condition);
        }
        printf("\n");
    } else if (record->book_id[0] != '\0') {
        printf("      %s (%s) for %s\n", record->book_id, record->book_title, record->member_id);
    }
    if (request->flag_reason[0] != '\0') {
        printf("      reason: %s\n", request->flag_reason);
    }
}

bool command_fraud_review(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    int suspicious = 0;
    int approved = 0;

    printf("Requests held for review (SUSPICIOUS)\n");
    for (int i = 0; i < library->pool.count; i++) {
        if (library->pool.items[i].status == REQUEST_SUSPICIOUS) {
            print_review_entry(&library->pool.items[i]);
            suspicious++;
        }
    }
    if (suspicious == 0) {
        printf("  none\n");
    }

    printf("Approved after review, waiting to be mined\n");
    for (int i = 0; i < library->pool.count; i++) {
        if (library->pool.items[i].status == REQUEST_APPROVED) {
            print_review_entry(&library->pool.items[i]);
            approved++;
        }
    }
    if (approved == 0) {
        printf("  none\n");
    }
    if (suspicious > 0) {
        printf("Use approve_suspicious <id> or reject_suspicious <id> with an ID or its first %d characters.\n",
               MIN_ID_PREFIX);
    }
    return true;
}

/* Reviewing is a librarian decision, so it needs the librarian's key unlocked. */
static int find_flagged_request(LibrarySystem *library, const char *id)
{
    int index = find_request(&library->pool, id, true);
    if (index < 0) {
        printf("ERROR: no single SUSPICIOUS request matches '%s' (use at least %d characters of its ID)\n", id,
               MIN_ID_PREFIX);
        return -1;
    }
    if (unlock_librarian(library) == NULL) {
        return -1;
    }
    return index;
}

/*
 * Approval only lifts the hold. The request is checked again like any other,
 * so a replayed request whose nonce is already used is still dropped.
 */
bool command_approve_suspicious(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    int index = find_flagged_request(library, args[0]);
    if (index < 0) {
        return false;
    }

    PendingRequest *request = &library->pool.items[index];
    request->status = REQUEST_APPROVED;
    printf("Approved request %.12s (%s by %s). It can now be mined if it is still valid.\n",
           request->record.record_id, action_name(request->record.action), request->record.signer_id);
    save_pool(POOL_FILE, &library->pool);
    prune_pool(library);
    return true;
}

bool command_reject_suspicious(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    int index = find_flagged_request(library, args[0]);
    if (index < 0) {
        return false;
    }

    const Record *record = &library->pool.items[index].record;
    printf("Rejected request %.12s (%s by %s). It is removed from the pool and never mined.\n", record->record_id,
           action_name(record->action), record->signer_id);
    pool_remove(&library->pool, index);
    save_pool(POOL_FILE, &library->pool);
    prune_pool(library);
    return true;
}

static const Record *find_signed_record(const LibrarySystem *library, const char *id)
{
    size_t length = strlen(id);
    if (length < MIN_ID_PREFIX) {
        return NULL;
    }
    for (int b = 0; b < library->chain.count; b++) {
        const Block *block = &library->chain.blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            if (strncmp(block->records[r].record_id, id, length) == 0) {
                return &block->records[r];
            }
        }
    }
    int index = find_request(&library->pool, id, false);
    if (index < 0) {
        index = find_request(&library->pool, id, true);
    }
    return index >= 0 ? &library->pool.items[index].record : NULL;
}

/*
 * Demonstrates replay protection: a copy of a request that was already
 * signed is submitted again, byte for byte. The duplicate check flags it,
 * and even if a librarian approved it, its used nonce would get it dropped.
 */
bool command_replay_request(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    if (!require_writable_chain(library)) {
        return false;
    }
    const Record *original = find_signed_record(library, args[0]);
    if (original == NULL) {
        printf("ERROR: no signed record on the chain or in the pool matches '%s'\n", args[0]);
        return false;
    }

    Record copy = *original;
    printf("Replaying %.12s (%s by %s, nonce %d) without changing a byte.\n", copy.record_id,
           action_name(copy.action), copy.signer_id, copy.nonce);
    return resubmit_signed_record(library, &copy);
}
