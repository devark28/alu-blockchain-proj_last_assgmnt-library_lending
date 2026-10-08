#include "ledger.h"

#include <stdio.h>
#include <string.h>

const char *model_name(LedgerModel model)
{
    return model == MODEL_UTXO ? "UTXO" : "account";
}

bool parse_model(const char *text, LedgerModel *model)
{
    if (strcmp(text, "utxo") == 0) {
        *model = MODEL_UTXO;
        return true;
    }
    if (strcmp(text, "account") == 0) {
        *model = MODEL_ACCOUNT;
        return true;
    }
    return false;
}

/*
 * Validates and applies one transaction in the session's model. In the UTXO
 * model a payment's inputs and change are chosen here, from the set as it
 * stands at this point in the chain.
 */
bool ledger_apply(Ledger *ledger, Transaction *tx, char problem[PROBLEM_SIZE])
{
    bool is_payment = !is_minting_transaction(tx);
    if (is_payment && strcmp(tx->sender, tx->outputs[0].owner) == 0) {
        snprintf(problem, PROBLEM_SIZE, "%s cannot pay themselves", tx->sender);
        return false;
    }

    bool applied;
    if (ledger->model == MODEL_UTXO) {
        applied = (!is_payment || utxo_select_inputs(&ledger->utxos, tx, problem))
                  && utxo_validate(&ledger->utxos, tx, problem)
                  && utxo_apply(&ledger->utxos, tx);
    } else {
        applied = account_validate(&ledger->accounts, tx, problem) && account_apply(&ledger->accounts, tx);
    }

    if (applied && is_payment) {
        ledger->last_payment = *tx;
        ledger->has_last_payment = true;
    }
    return applied;
}

int ledger_balance(const Ledger *ledger, const char *id)
{
    if (ledger->model == MODEL_UTXO) {
        return utxo_balance(&ledger->utxos, id);
    }
    const Account *account = find_account(&ledger->accounts, id);
    return account != NULL ? account->balance : 0;
}

bool ledger_clone(const Ledger *source, Ledger *copy)
{
    memset(copy, 0, sizeof(*copy));
    copy->model = source->model;
    copy->has_last_payment = source->has_last_payment;
    copy->last_payment = source->last_payment;
    if (!utxo_clone(&source->utxos, &copy->utxos)) {
        return false;
    }
    if (!account_table_clone(&source->accounts, &copy->accounts)) {
        utxo_set_free(&copy->utxos);
        return false;
    }
    return true;
}

void ledger_print_state(const Ledger *ledger, const Keyring *identities)
{
    if (ledger->model == MODEL_UTXO) {
        utxo_print_set(&ledger->utxos, NULL);
    } else {
        account_print_table(&ledger->accounts, identities);
    }
}

void ledger_free(Ledger *ledger)
{
    utxo_set_free(&ledger->utxos);
    account_table_free(&ledger->accounts);
}
