#ifndef MINING_H
#define MINING_H

#include "library.h"

#include <stdbool.h>

int assemble_block(LibrarySystem *library, Block *block, LibraryState **working);
void set_block_rewards(Block *block, const TxOutput *payouts, int payout_count);
bool commit_block(LibrarySystem *library, const Block *block, LibraryState *working);
int count_minable_requests(const LibrarySystem *library);

#endif
