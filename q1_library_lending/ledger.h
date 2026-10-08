#ifndef LEDGER_H
#define LEDGER_H

#include "account.h"
#include "keyring.h"
#include "transaction.h"
#include "utxo.h"

#include <stdbool.h>

typedef enum {
    MODEL_UTXO,
    MODEL_ACCOUNT
} LedgerModel;

/*
 * Token balances in the model chosen for the session. Only that model's
 * structure is filled; both are rebuilt from the chain on every start, so
 * switching models never loses a balance. last_payment keeps the latest
 * confirmed payment with its inputs for the double-spend test.
 */
typedef struct {
    LedgerModel model;
    UtxoSet utxos;
    AccountTable accounts;
    bool has_last_payment;
    Transaction last_payment;
} Ledger;

const char *model_name(LedgerModel model);
bool parse_model(const char *text, LedgerModel *model);

bool ledger_apply(Ledger *ledger, Transaction *tx, char problem[PROBLEM_SIZE]);
int ledger_balance(const Ledger *ledger, const char *id);
bool ledger_clone(const Ledger *source, Ledger *copy);
void ledger_print_state(const Ledger *ledger, const Keyring *identities);
void ledger_free(Ledger *ledger);

#endif
