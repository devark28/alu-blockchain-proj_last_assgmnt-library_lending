#include "mining.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int count_minable_requests(const LibrarySystem *library)
{
    int count = 0;
    for (int i = 0; i < library->pool.count; i++) {
        if (library->pool.items[i].status != REQUEST_SUSPICIOUS) {
            count++;
        }
    }
    return count;
}

/*
 * A request can go into the block being built when its signer is known, it
 * carries the signer's next nonce, its signature verifies and the lending
 * rules accept it on top of the records already chosen. Otherwise it waits:
 * a member's second request, or a request from a member whose registration
 * is in this same block, becomes eligible on a later pass.
 */
static bool try_to_include(LibraryState *working, const Record *record)
{
    const Identity *signer = find_identity(&working->identities, record->signer_id);
    if (signer == NULL || record->nonce != signer->next_nonce
        || !record_signature_is_valid(record, signer->public_key)) {
        return false;
    }
    char problem[PROBLEM_SIZE];
    return apply_record(working, record, problem);
}

/*
 * Fills block with up to RECORDS_PER_BLOCK requests, taken from the pool in
 * priority order. Requests flagged SUSPICIOUS are never taken. Returns how
 * many records were chosen; working receives the state with them applied.
 */
int assemble_block(LibrarySystem *library, Block *block, LibraryState **working)
{
    memset(block, 0, sizeof(*block));
    *working = NULL;
    if (library->pool.count == 0) {
        return 0;
    }

    int *order = malloc((size_t)library->pool.count * sizeof(int));
    bool *chosen = calloc((size_t)library->pool.count, sizeof(bool));
    LibraryState *state = clone_state(&library->state);
    if (order == NULL || chosen == NULL || state == NULL) {
        free(order);
        free(chosen);
        free_state(state);
        return 0;
    }
    sort_pool_order(&library->pool, order);

    bool progress = true;
    while (progress && block->record_count < RECORDS_PER_BLOCK) {
        progress = false;
        for (int i = 0; i < library->pool.count && block->record_count < RECORDS_PER_BLOCK; i++) {
            const PendingRequest *request = &library->pool.items[order[i]];
            if (chosen[order[i]] || request->status == REQUEST_SUSPICIOUS) {
                continue;
            }
            if (try_to_include(state, &request->record)) {
                block->records[block->record_count++] = request->record;
                chosen[order[i]] = true;
                progress = true;
            }
        }
    }

    free(order);
    free(chosen);
    if (block->record_count == 0) {
        free_state(state);
        return 0;
    }
    *working = state;
    return block->record_count;
}

/*
 * Called once the block's proof of work is found. The block is appended and
 * saved, the state that already includes its records replaces the old one,
 * and its requests leave the pool. Takes ownership of working.
 */
bool commit_block(LibrarySystem *library, const Block *block, LibraryState *working)
{
    if (!block_list_append(&library->chain, block)) {
        free_state(working);
        return false;
    }
    if (!save_chain(CHAIN_FILE, &library->chain)) {
        library->chain.count--;
        free_state(working);
        return false;
    }

    free_state_contents(&library->state);
    library->state = *working;
    free(working);

    for (int r = 0; r < block->record_count; r++) {
        int index = find_request(&library->pool, block->records[r].record_id, false);
        if (index >= 0) {
            pool_remove(&library->pool, index);
        }
    }
    save_pool(POOL_FILE, &library->pool);
    prune_pool(library);
    return true;
}
