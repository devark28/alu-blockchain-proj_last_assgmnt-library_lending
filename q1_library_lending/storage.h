#ifndef STORAGE_H
#define STORAGE_H

#include "blockchain.h"
#include "ledger.h"
#include "state.h"

#include <stdbool.h>

#define INVENTORY_FILE "inventory.dat"
#define RESERVATIONS_FILE "reservations.dat"
#define LEDGER_FILE "ledger.dat"
#define META_FILE "meta.dat"

/* What meta.dat records about the chain when it was last saved. */
typedef struct {
    bool found;
    int difficulty;
    int block_reward;
    LedgerModel model;
    int height;
    char tip_hash[HASH_HEX_SIZE];
} ChainMetadata;

bool load_metadata(ChainMetadata *metadata);
bool check_chain_against_metadata(const BlockList *chain, const ChainMetadata *metadata);
bool save_snapshots(const LibraryState *state, const BlockList *chain, int difficulty);
void compare_snapshots(const LibraryState *state);

#endif
