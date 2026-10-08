#ifndef STATE_H
#define STATE_H

#include "blockchain.h"
#include "keyring.h"
#include "ledger.h"
#include "record.h"
#include "registry.h"
#include "reservation.h"

#include <stdbool.h>
#include <time.h>

/* Length of one "day" in seconds. A test build can shorten it with -DDAY_SECONDS=10. */
#ifndef DAY_SECONDS
#define DAY_SECONDS 86400
#endif

#define LOAN_DAYS 14
#define RENEWAL_DAYS 7
#define MAX_RENEWALS 2
#define FINE_PER_DAY 2

/*
 * Everything that follows from the confirmed chain: copy states and fines
 * (inside registry), identities and nonces, reservation queues and token
 * balances. current_block is the index of the block whose records are
 * being applied, which the transaction history records.
 */
typedef struct {
    Registry registry;
    Keyring identities;
    ReservationList reservations;
    Ledger ledger;
    int current_block;
} LibraryState;

LibraryState *clone_state(const LibraryState *state);
void free_state_contents(LibraryState *state);
void free_state(LibraryState *state);

bool apply_record(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE]);
bool apply_block_transactions(LibraryState *state, const Block *block, char problem[PROBLEM_SIZE]);
bool replay_chain(LibraryState *state, const BlockList *chain);

int compute_fine(time_t due_date, time_t at);
time_t loan_due_date(time_t borrowed_at);
bool is_valid_reason_code(const char *code);
void print_reason_codes(void);

#endif
