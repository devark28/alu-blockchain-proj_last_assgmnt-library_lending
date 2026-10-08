#ifndef MERKLE_H
#define MERKLE_H

#include "crypto.h"

#include <stdbool.h>

#define MAX_MERKLE_LEAVES 16

bool compute_merkle_root(const char leaves[][HASH_HEX_SIZE], int leaf_count, char root[HASH_HEX_SIZE]);
void print_merkle_tree(const char leaves[][HASH_HEX_SIZE], int leaf_count);

#endif
