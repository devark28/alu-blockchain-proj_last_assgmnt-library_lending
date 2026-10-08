#include "commands.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MAX_TRANSFER_AMOUNT 1000000

static bool parse_amount(const char *text, int *amount)
{
    long long value;
    if (!parse_number(text, 1, MAX_TRANSFER_AMOUNT, &value)) {
        printf("ERROR: amount must be a whole number from 1 to %d\n", MAX_TRANSFER_AMOUNT);
        return false;
    }
    *amount = (int)value;
    return true;
}

static bool require_model(const LibrarySystem *library, LedgerModel model)
{
    if (library->state.ledger.model != model) {
        printf("ERROR: this command needs the %s model. Start the program with --model %s\n", model_name(model),
               model == MODEL_UTXO ? "utxo" : "account");
        return false;
    }
    return true;
}

/* A payment record is signed by the paying member; its txid becomes the record_id. */
static void make_payment_record(Record *record, RecordAction action, const Member *payer, const char *recipient_id,
                                int amount)
{
    memset(record, 0, sizeof(*record));
    record->action = action;
    record->timestamp = time(NULL);
    copy_field(record->signer_id, ID_SIZE, payer->member_id);
    copy_field(record->member_id, ID_SIZE, payer->member_id);
    copy_field(record->member_name, NAME_SIZE, payer->full_name);
    copy_field(record->recipient_id, ID_SIZE, recipient_id);
    record->amount = amount;
}

static bool submit_transfer(LibrarySystem *library, char *args[], int nonce)
{
    const Registry *registry = &library->state.registry;
    const Member *sender = require_member(registry, args[0]);
    if (sender == NULL || require_member(registry, args[1]) == NULL) {
        return false;
    }
    int amount;
    if (!parse_amount(args[2], &amount)) {
        return false;
    }

    EVP_PKEY *sender_key = unlock_member(library, sender->member_id);
    if (sender_key == NULL) {
        return false;
    }
    Record record;
    make_payment_record(&record, ACTION_TRANSFER, sender, args[1], amount);
    if (!submit_request_with_nonce(library, &record, sender_key, nonce)) {
        return false;
    }
    printf("%s pays %d to %s plus a fee of %d once the request is confirmed.\n", sender->member_id, amount, args[1],
           TRANSACTION_FEE);
    return true;
}

bool command_token_transfer(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    return submit_transfer(library, args, AUTOMATIC_NONCE);
}

/*
 * The account-model transfer takes the nonce from the user instead of
 * choosing it, so a wrong or already used nonce can be seen being rejected.
 */
bool command_account_transfer(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    long long nonce;
    if (!require_model(library, MODEL_ACCOUNT)) {
        return false;
    }
    if (!parse_number(args[3], 0, INT_MAX, &nonce)) {
        printf("ERROR: nonce must be a whole number from 0\n");
        return false;
    }
    return submit_transfer(library, args, (int)nonce);
}

bool command_token_balance(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    if (require_member(&library->state.registry, args[0]) == NULL) {
        return false;
    }
    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return false;
    }

    int confirmed = ledger_balance(&library->state.ledger, args[0]);
    int after_pending = ledger_balance(&effective->ledger, args[0]);
    printf("%s balance (%s model): %d confirmed", args[0], model_name(library->state.ledger.model), confirmed);
    if (after_pending != confirmed) {
        printf(", %d after pending payments", after_pending);
    }
    printf("\n");
    free_state(effective);
    return true;
}

bool command_utxo_view(LibrarySystem *library, int arg_count, char *args[])
{
    if (!require_model(library, MODEL_UTXO)) {
        return false;
    }
    if (arg_count == 1 && require_member(&library->state.registry, args[0]) == NULL) {
        return false;
    }
    utxo_print_set(&library->state.ledger.utxos, arg_count == 1 ? args[0] : NULL);
    return true;
}

bool command_account_balance(LibrarySystem *library, int arg_count, char *args[])
{
    const LibraryState *state = &library->state;
    if (!require_model(library, MODEL_ACCOUNT)) {
        return false;
    }
    if (arg_count == 0) {
        account_print_table(&state->ledger.accounts, &state->identities);
        return true;
    }
    if (require_member(&state->registry, args[0]) == NULL) {
        return false;
    }
    const Identity *identity = find_identity(&state->identities, args[0]);
    printf("%s: balance %d, next nonce %d\n", args[0], ledger_balance(&state->ledger, args[0]),
           identity != NULL ? identity->next_nonce : 0);
    return true;
}

bool command_transaction_history(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const LibraryState *state = &library->state;
    if (!require_model(library, MODEL_ACCOUNT) || require_member(&state->registry, args[0]) == NULL) {
        return false;
    }
    const Account *account = find_account(&state->ledger.accounts, args[0]);
    if (account == NULL) {
        printf("%s has no token transactions yet.\n", args[0]);
        return true;
    }
    account_print_history(account, &state->identities);
    return true;
}

/* The whole fine owed is paid to the library in one FINE_PAID request signed by the member. */
bool command_settle_fine(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const Member *member = require_member(&library->state.registry, args[0]);
    if (member == NULL) {
        return false;
    }
    if (member->fines_owed == 0) {
        printf("%s owes no fines.\n", member->member_id);
        return true;
    }

    EVP_PKEY *member_key = unlock_member(library, member->member_id);
    if (member_key == NULL) {
        return false;
    }
    Record record;
    make_payment_record(&record, ACTION_FINE_PAID, member, LIBRARY_ACCOUNT, member->fines_owed);
    if (!submit_request(library, &record, member_key)) {
        return false;
    }
    printf("%s pays the %d coin fine plus a fee of %d once the request is confirmed.\n", member->member_id,
           record.amount, TRANSACTION_FEE);
    return true;
}

/*
 * Tries to spend the inputs of the last confirmed payment a second time.
 * They left the UTXO set when the payment was confirmed, so the check that
 * every input must still be unspent rejects it.
 */
bool command_double_spend_test(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    const Ledger *ledger = &library->state.ledger;
    if (!require_model(library, MODEL_UTXO)) {
        printf("In the account model a replay is stopped by the nonce instead; try account_transfer with a used "
               "nonce, or replay_request.\n");
        return false;
    }
    if (!ledger->has_last_payment) {
        printf("No payment has been confirmed yet. Mine a token_transfer first.\n");
        return false;
    }

    const Transaction *payment = &ledger->last_payment;
    printf("Spending the inputs of payment %.12s again (%s, %d input%s):\n", payment->txid, payment->sender,
           payment->input_count, payment->input_count == 1 ? "" : "s");
    for (int i = 0; i < payment->input_count; i++) {
        printf("  input %.16s...:%d\n", payment->inputs[i].txid, payment->inputs[i].output_index);
    }

    char problem[PROBLEM_SIZE];
    if (utxo_validate(&ledger->utxos, payment, problem)) {
        printf("UNEXPECTED: the inputs are still unspent\n");
        return false;
    }
    printf("REJECTED: %s\n", problem);
    printf("Double spend prevented.\n");
    return true;
}
