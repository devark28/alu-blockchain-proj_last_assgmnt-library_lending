#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include "crypto.h"
#include "keyring.h"
#include "record.h"

#include <stdbool.h>
#include <time.h>

#define RECORDS_PER_BLOCK 4
#define MAX_PAYOUTS 6
#define DEFAULT_DIFFICULTY 2
#define MIN_DIFFICULTY 1
#define MAX_DIFFICULTY 4

typedef struct {
    char account_id[ID_SIZE];
    int amount;
} Payout;

/*
 * The header fields are hashed together into hash. The records are covered
 * through merkle_root, and payouts lists who received the block reward so
 * that pool splits can be rebuilt from the chain.
 */
typedef struct {
    int index;
    time_t timestamp;
    int record_count;
    char previous_hash[HASH_HEX_SIZE];
    char merkle_root[HASH_HEX_SIZE];
    char hash[HASH_HEX_SIZE];
    char sealed_by[ID_SIZE];
    int token_reward;
    char transaction_id[HASH_HEX_SIZE];
    int difficulty;
    unsigned long nonce;
    int block_reward;
    Payout payouts[MAX_PAYOUTS];
    int payout_count;
    Record records[RECORDS_PER_BLOCK];
} Block;

typedef struct {
    Block *blocks;
    int count;
    int capacity;
} BlockList;

bool block_list_append(BlockList *list, const Block *block);
void block_list_free(BlockList *list);
const Block *last_block(const BlockList *chain);

bool compute_block_merkle_root(const Block *block, char root[HASH_HEX_SIZE]);
void compute_block_hash(const Block *block, char output[HASH_HEX_SIZE]);
bool hash_meets_difficulty(const char *hash, int difficulty);
void prepare_block(Block *block, const BlockList *chain, int difficulty, const char *sealed_by);
bool search_nonce(Block *block, unsigned long max_attempts, unsigned long *attempts);

bool validate_chain(const BlockList *chain, Keyring *identities, bool print_report);

bool save_chain(const char *path, const BlockList *chain);
bool load_chain(const char *path, BlockList *chain);

void print_block_summary(const Block *block);
void print_block_details(const Block *block);

#endif
