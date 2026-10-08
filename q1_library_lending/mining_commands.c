#include "commands.h"
#include "mining.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char id[ID_SIZE];
    int hash_rate;
    unsigned long attempts;
    unsigned long total_attempts;
    int blocks_found;
    int total_reward;
} PoolMiner;

static bool ready_to_mine(const LibrarySystem *library, const char *member_id)
{
    if (require_member(&library->state.registry, member_id) == NULL || !require_writable_chain(library)) {
        return false;
    }
    if (count_minable_requests(library) == 0) {
        printf("Nothing to mine: the pending pool has no requests that are not held for review.\n");
        return false;
    }
    return true;
}

static double elapsed_ms(clock_t start)
{
    return (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
}

static void print_block_found(const Block *block, unsigned long attempts)
{
    printf("Block %d: %d record%s, nonce %lu found after %lu hash attempts\n  hash %s\n", block->index,
           block->record_count, block->record_count == 1 ? "" : "s", block->nonce, attempts, block->hash);
}

/*
 * One miner confirms every pending request, a block at a time, searching
 * nonces until each hash meets the difficulty. The miner receives the whole
 * BLOCK_REWARD for every block, on top of the members' token rewards.
 */
bool command_mine_solo(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const char *miner_id = args[0];
    if (!ready_to_mine(library, miner_id)) {
        return false;
    }
    printf("Solo miner %s, difficulty %d: a hash must start with %d zero%s.\n", miner_id, library->difficulty,
           library->difficulty, library->difficulty == 1 ? "" : "s");

    TxOutput payout = {.amount = BLOCK_REWARD};
    copy_field(payout.owner, ID_SIZE, miner_id);
    int blocks_mined = 0;
    unsigned long total_attempts = 0;
    Block block;
    LibraryState *working;

    while (assemble_block(library, &block, &working) > 0) {
        prepare_block(&block, &library->chain, library->difficulty, miner_id);
        set_block_rewards(&block, &payout, 1);
        unsigned long attempts = 0;
        clock_t start = clock();
        search_nonce(&block, ULONG_MAX, &attempts);
        double milliseconds = elapsed_ms(start);
        if (!commit_block(library, &block, working)) {
            return false;
        }
        print_block_found(&block, attempts);
        printf("  %.1f ms, %d coins to %s\n", milliseconds, BLOCK_REWARD, miner_id);
        blocks_mined++;
        total_attempts += attempts;
    }

    printf("Solo mining: %d block%s, %lu hash attempts in total, %d coins to %s.\n", blocks_mined,
           blocks_mined == 1 ? "" : "s", total_attempts, blocks_mined * BLOCK_REWARD, miner_id);
    ledger_print_state(&library->state.ledger, &library->state.identities);
    return blocks_mined > 0;
}

/* Random hash rates in steps of POOL_SLICES_PER_ROUND, so every slice gets a whole number of attempts. */
static void set_up_pool(PoolMiner miners[POOL_MINER_COUNT], const char *member_id)
{
    int rate_steps = (POOL_MAX_HASH_RATE - POOL_MIN_HASH_RATE) / POOL_SLICES_PER_ROUND + 1;
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        memset(&miners[i], 0, sizeof(miners[i]));
        if (i == 0) {
            copy_field(miners[i].id, ID_SIZE, member_id);
        } else {
            snprintf(miners[i].id, ID_SIZE, "%s%d", POOL_PEER_PREFIX, i);
        }
        miners[i].hash_rate = POOL_MIN_HASH_RATE + (rand() % rate_steps) * POOL_SLICES_PER_ROUND;
    }
}

/*
 * Splits one block reward by contribution: the pool fee comes off first,
 * then each miner gets (attempts / total attempts) of the rest, rounded down,
 * with the rounding remainder going to the miner with the most attempts.
 */
static int split_pool_reward(const PoolMiner miners[POOL_MINER_COUNT], TxOutput payouts[MAX_PAYOUTS],
                             int rewards[POOL_MINER_COUNT])
{
    int pool_fee = BLOCK_REWARD * POOL_FEE_PERCENT / 100;
    int shared = BLOCK_REWARD - pool_fee;
    unsigned long total_attempts = 0;
    int top_miner = 0;
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        total_attempts += miners[i].attempts;
        if (miners[i].attempts > miners[top_miner].attempts) {
            top_miner = i;
        }
    }

    int distributed = 0;
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        rewards[i] = total_attempts == 0 ? 0 : (int)((unsigned long long)shared * miners[i].attempts / total_attempts);
        distributed += rewards[i];
    }
    rewards[top_miner] += shared - distributed;

    int count = 0;
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        if (rewards[i] > 0) {
            copy_field(payouts[count].owner, ID_SIZE, miners[i].id);
            payouts[count].amount = rewards[i];
            count++;
        }
    }
    copy_field(payouts[count].owner, ID_SIZE, POOL_OPERATOR_ACCOUNT);
    payouts[count].amount = pool_fee;
    return count + 1;
}

/*
 * The block's payouts and sealer are part of the hashed header, so they have
 * to be fixed before each hash. It is recomputed from the attempts made so far,
 * this one included, so the block that meets the target pays exactly for
 * the work done up to that moment.
 */
static bool pool_attempts(Block *block, PoolMiner miners[POOL_MINER_COUNT], int miner, unsigned long count)
{
    TxOutput payouts[MAX_PAYOUTS];
    int rewards[POOL_MINER_COUNT];
    copy_field(block->sealed_by, ID_SIZE, miners[miner].id);
    for (unsigned long tried = 0; tried < count; tried++) {
        miners[miner].attempts++;
        int payout_count = split_pool_reward(miners, payouts, rewards);
        set_block_rewards(block, payouts, payout_count);
        compute_block_hash(block, block->hash);
        if (hash_meets_difficulty(block->hash, block->difficulty)) {
            return true;
        }
        block->nonce++;
    }
    return false;
}

/*
 * Each round is split into time slices, and in every slice each miner tries
 * a tenth of its hash rate. Faster miners therefore do proportionally more
 * work and no miner always goes first. All miners continue one shared nonce
 * sequence, so no work is repeated.
 */
static int pool_search(Block *block, PoolMiner miners[POOL_MINER_COUNT], int *rounds)
{
    *rounds = 0;
    while (true) {
        (*rounds)++;
        for (int slice = 0; slice < POOL_SLICES_PER_ROUND; slice++) {
            for (int i = 0; i < POOL_MINER_COUNT; i++) {
                unsigned long slice_attempts = (unsigned long)(miners[i].hash_rate / POOL_SLICES_PER_ROUND);
                if (pool_attempts(block, miners, i, slice_attempts)) {
                    return i;
                }
            }
        }
    }
}

static void print_pool_table(const PoolMiner miners[POOL_MINER_COUNT], const int rewards[POOL_MINER_COUNT],
                             bool session_totals)
{
    unsigned long total = 0;
    int shared = 0;
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        total += session_totals ? miners[i].total_attempts : miners[i].attempts;
        shared += rewards[i];
    }

    printf("  %-10s %9s %10s %8s %7s\n", "miner", "hash rate", "attempts", "share", "reward");
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        unsigned long attempts = session_totals ? miners[i].total_attempts : miners[i].attempts;
        double share = total == 0 ? 0.0 : 100.0 * (double)attempts / (double)total;
        printf("  %-10s %9d %10lu %7.2f%% %7d\n", miners[i].id, miners[i].hash_rate, attempts, share, rewards[i]);
    }
    printf("  %-10s %9s %10lu %7.2f%% %7d\n", "total", "", total, 100.0, shared);
}

bool command_mine_pool(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    if (!ready_to_mine(library, args[0])) {
        return false;
    }

    PoolMiner miners[POOL_MINER_COUNT];
    set_up_pool(miners, args[0]);
    int pool_fee = BLOCK_REWARD * POOL_FEE_PERCENT / 100;
    printf("Mining pool at difficulty %d. Each block pays %d: a %d%% pool fee of %d to %s, and %d shared by attempts.\n",
           library->difficulty, BLOCK_REWARD, POOL_FEE_PERCENT, pool_fee, POOL_OPERATOR_ACCOUNT,
           BLOCK_REWARD - pool_fee);

    int blocks_mined = 0;
    Block block;
    LibraryState *working;
    while (assemble_block(library, &block, &working) > 0) {
        prepare_block(&block, &library->chain, library->difficulty, POOL_OPERATOR_ACCOUNT);
        for (int i = 0; i < POOL_MINER_COUNT; i++) {
            miners[i].attempts = 0;
        }

        int rounds;
        int finder = pool_search(&block, miners, &rounds);
        if (!commit_block(library, &block, working)) {
            return false;
        }

        TxOutput payouts[MAX_PAYOUTS];
        int rewards[POOL_MINER_COUNT];
        split_pool_reward(miners, payouts, rewards);
        printf("\nBlock %d found by %s in round %d, nonce %lu\n  hash %s\n", block.index, miners[finder].id, rounds,
               block.nonce, block.hash);
        print_pool_table(miners, rewards, false);

        miners[finder].blocks_found++;
        for (int i = 0; i < POOL_MINER_COUNT; i++) {
            miners[i].total_attempts += miners[i].attempts;
            miners[i].total_reward += rewards[i];
        }
        blocks_mined++;
    }

    int totals[POOL_MINER_COUNT];
    for (int i = 0; i < POOL_MINER_COUNT; i++) {
        totals[i] = miners[i].total_reward;
    }
    printf("\nPool session: %d block%s, %d coins to %s in pool fees\n", blocks_mined, blocks_mined == 1 ? "" : "s",
           blocks_mined * pool_fee, POOL_OPERATOR_ACCOUNT);
    print_pool_table(miners, totals, true);
    ledger_print_state(&library->state.ledger, &library->state.identities);
    return blocks_mined > 0;
}

/* The renter pays the whole rental up front, as one signed transfer to the cloud provider. */
static bool pay_rental(LibrarySystem *library, const Member *renter, int rounds)
{
    EVP_PKEY *renter_key = unlock_member(library, renter->member_id);
    if (renter_key == NULL) {
        return false;
    }
    Record payment;
    memset(&payment, 0, sizeof(payment));
    payment.action = ACTION_TRANSFER;
    payment.timestamp = time(NULL);
    copy_field(payment.signer_id, ID_SIZE, renter->member_id);
    copy_field(payment.member_id, ID_SIZE, renter->member_id);
    copy_field(payment.member_name, NAME_SIZE, renter->full_name);
    copy_field(payment.recipient_id, ID_SIZE, CLOUD_PROVIDER_ACCOUNT);
    payment.amount = rounds * CLOUD_RENTAL_FEE;
    return submit_request(library, &payment, renter_key);
}

static void print_cloud_summary(int gross, int rental_fees, int maintenance_fees)
{
    int fees = rental_fees + maintenance_fees;
    printf("\nCloud mining summary\n");
    printf("  Gross earnings  %d coins\n", gross);
    printf("  Fees paid       %d coins (rental %d + maintenance %d)\n", fees, rental_fees, maintenance_fees);
    printf("  Net profit      %d coins\n", gross - fees);
}

/*
 * A member rents CLOUD_HASH_RATE attempts per round for 1 to 5 rounds,
 * paying CLOUD_RENTAL_FEE per round up front. A block found with the rented
 * power pays BLOCK_REWARD, of which the provider keeps a maintenance share.
 * After every round the running totals are shown, with a warning whenever
 * the fees so far exceed the rewards so far.
 */
bool command_mine_cloud(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    long long rounds;
    if (!parse_number(args[0], MIN_RENTAL_ROUNDS, MAX_RENTAL_ROUNDS, &rounds)) {
        printf("ERROR: the rental lasts %d to %d rounds\n", MIN_RENTAL_ROUNDS, MAX_RENTAL_ROUNDS);
        return false;
    }
    const Member *renter = require_member(&library->state.registry, args[1]);
    if (renter == NULL || !require_writable_chain(library)) {
        return false;
    }

    int maintenance_fee = BLOCK_REWARD * CLOUD_MAINTENANCE_PERCENT / 100;
    printf("Cloud rental for %s: %lld round%s of %d attempts at %d coins per round, maintenance %d%% of each "
           "block reward, difficulty %d.\n", renter->member_id, rounds, rounds == 1 ? "" : "s", CLOUD_HASH_RATE,
           CLOUD_RENTAL_FEE, CLOUD_MAINTENANCE_PERCENT, library->difficulty);
    if (!pay_rental(library, renter, (int)rounds)) {
        printf("ERROR: the rental could not be paid, so no mining power was rented\n");
        return false;
    }

    TxOutput payouts[2] = {{.amount = BLOCK_REWARD - maintenance_fee}, {.amount = maintenance_fee}};
    copy_field(payouts[0].owner, ID_SIZE, renter->member_id);
    copy_field(payouts[1].owner, ID_SIZE, CLOUD_PROVIDER_ACCOUNT);

    int gross = 0;
    int rental_fees = 0;
    int maintenance_fees = 0;
    Block block;
    LibraryState *working = NULL;
    bool have_block = false;

    for (int round = 1; round <= rounds; round++) {
        rental_fees += CLOUD_RENTAL_FEE;
        if (!have_block && assemble_block(library, &block, &working) > 0) {
            prepare_block(&block, &library->chain, library->difficulty, renter->member_id);
            set_block_rewards(&block, payouts, 2);
            have_block = true;
        }

        unsigned long attempts = 0;
        if (!have_block) {
            printf("Round %d: nothing left to mine, the rented power is idle\n", round);
        } else if (search_nonce(&block, CLOUD_HASH_RATE, &attempts)) {
            if (!commit_block(library, &block, working)) {
                return false;
            }
            have_block = false;
            gross += BLOCK_REWARD;
            maintenance_fees += maintenance_fee;
            printf("Round %d: ", round);
            print_block_found(&block, block.nonce + 1);
        } else {
            printf("Round %d: no valid hash in %lu attempts, block %d carries over\n", round, attempts, block.index);
        }

        int fees_so_far = rental_fees + maintenance_fees;
        printf("  so far: rewards %d, fees %d, net %d\n", gross, fees_so_far, gross - fees_so_far);
        if (fees_so_far > gross) {
            printf("  WARNING: fees exceed rewards after round %d, the rental is unprofitable so far\n", round);
        }
    }

    if (have_block) {
        free_state(working);
        printf("The unfinished block's requests stay in the pending pool.\n");
    }
    print_cloud_summary(gross, rental_fees, maintenance_fees);
    ledger_print_state(&library->state.ledger, &library->state.identities);
    return true;
}

/* Each extra leading zero makes a valid hash 16 times rarer, since a hex digit has 16 values. */
static unsigned long expected_attempts(int difficulty)
{
    unsigned long attempts = 1;
    for (int i = 0; i < difficulty; i++) {
        attempts *= 16;
    }
    return attempts;
}

bool command_difficulty_status(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    printf("Current difficulty %d: a hash must start with %d zero%s, about %lu attempts on average.\n",
           library->difficulty, library->difficulty, library->difficulty == 1 ? "" : "s",
           expected_attempts(library->difficulty));
    printf("  difficulty  expected attempts\n");
    for (int d = MIN_DIFFICULTY; d <= MAX_DIFFICULTY; d++) {
        printf("  %-10d  %lu\n", d, expected_attempts(d));
    }

    const BlockList *chain = &library->chain;
    int first = chain->count > 10 ? chain->count - 10 : 0;
    printf("Recent blocks (attempts = nonce + 1, since every method counts nonces up from 0)\n");
    printf("  %-5s %-10s %-10s %s\n", "block", "difficulty", "attempts", "sealed by");
    for (int i = first; i < chain->count; i++) {
        const Block *block = &chain->blocks[i];
        printf("  %-5d %-10d %-10lu %s\n", block->index, block->difficulty, block->nonce + 1, block->sealed_by);
    }
    return true;
}

bool command_set_difficulty(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    long long difficulty;
    if (!parse_number(args[0], MIN_DIFFICULTY, MAX_DIFFICULTY, &difficulty)) {
        printf("ERROR: difficulty must be from %d to %d\n", MIN_DIFFICULTY, MAX_DIFFICULTY);
        return false;
    }
    if (unlock_librarian(library) == NULL) {
        return false;
    }
    library->difficulty = (int)difficulty;
    printf("Difficulty set to %d. New blocks need a hash starting with %d zero%s (about %lu attempts).\n",
           library->difficulty, library->difficulty, library->difficulty == 1 ? "" : "s",
           expected_attempts(library->difficulty));
    return true;
}
