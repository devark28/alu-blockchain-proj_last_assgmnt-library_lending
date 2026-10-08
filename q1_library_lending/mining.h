#ifndef MINING_H
#define MINING_H

#include "library.h"

#include <stdbool.h>

#define POOL_MINER_COUNT 4
#define POOL_MIN_HASH_RATE 50
#define POOL_MAX_HASH_RATE 300
#define POOL_SLICES_PER_ROUND 10
#define POOL_FEE_PERCENT 2
#define POOL_PEER_PREFIX "PEER-"
#define CLOUD_HASH_RATE 300
#define CLOUD_RENTAL_FEE 5
#define CLOUD_MAINTENANCE_PERCENT 10
#define MIN_RENTAL_ROUNDS 1
#define MAX_RENTAL_ROUNDS 5

int assemble_block(LibrarySystem *library, Block *block, LibraryState **working);
void set_block_rewards(Block *block, const TxOutput *payouts, int payout_count);
bool commit_block(LibrarySystem *library, const Block *block, LibraryState *working);
int count_minable_requests(const LibrarySystem *library);

#endif
