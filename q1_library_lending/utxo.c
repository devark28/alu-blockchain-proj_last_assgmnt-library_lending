#include "utxo.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

static int find_utxo(const UtxoSet *set, const char *txid, int output_index)
{
    for (int i = 0; i < set->count; i++) {
        if (set->items[i].output_index == output_index && strcmp(set->items[i].txid, txid) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Chooses the inputs for a payment: the sender's oldest outputs are spent
 * first until they cover the amount plus the fee, and anything above that
 * comes back to the sender as a change output. The choice is deterministic,
 * so replaying the chain always spends the same outputs.
 */
bool utxo_select_inputs(const UtxoSet *set, Transaction *tx, char problem[PROBLEM_SIZE])
{
    int needed = output_total(tx) + tx->fee;
    int collected = 0;
    tx->input_count = 0;

    for (int i = 0; i < set->count && collected < needed; i++) {
        const Utxo *utxo = &set->items[i];
        if (strcmp(utxo->owner, tx->sender) != 0) {
            continue;
        }
        if (tx->input_count == MAX_TX_INPUTS) {
            snprintf(problem, PROBLEM_SIZE, "%s would need more than %d inputs", tx->sender, MAX_TX_INPUTS);
            return false;
        }
        TxInput *input = &tx->inputs[tx->input_count++];
        copy_field(input->txid, HASH_HEX_SIZE, utxo->txid);
        input->output_index = utxo->output_index;
        collected += utxo->amount;
    }

    if (collected < needed) {
        snprintf(problem, PROBLEM_SIZE, "insufficient funds: %s has %d coins in unspent outputs but needs %d (%d + %d fee)",
                 tx->sender, utxo_balance(set, tx->sender), needed, output_total(tx), tx->fee);
        return false;
    }
    if (collected > needed && tx->output_count < MAX_TX_OUTPUTS) {
        TxOutput *change = &tx->outputs[tx->output_count++];
        copy_field(change->owner, ID_SIZE, tx->sender);
        change->amount = collected - needed;
    }
    return true;
}

/*
 * Every input must be an unspent output owned by the sender and used only
 * once, and the inputs must cover the outputs plus the fee. An input that
 * was already spent is no longer in the set, which is what stops a double
 * spend.
 */
bool utxo_validate(const UtxoSet *set, const Transaction *tx, char problem[PROBLEM_SIZE])
{
    for (int i = 0; i < tx->output_count; i++) {
        if (tx->outputs[i].amount <= 0) {
            snprintf(problem, PROBLEM_SIZE, "output %d has a non-positive amount", i);
            return false;
        }
    }
    if (is_minting_transaction(tx)) {
        return true;
    }

    int input_total = 0;
    for (int i = 0; i < tx->input_count; i++) {
        const TxInput *input = &tx->inputs[i];
        int position = find_utxo(set, input->txid, input->output_index);
        if (position < 0) {
            snprintf(problem, PROBLEM_SIZE, "input %.12s...:%d is not in the UTXO set (already spent or never existed)",
                     input->txid, input->output_index);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (tx->inputs[j].output_index == input->output_index && strcmp(tx->inputs[j].txid, input->txid) == 0) {
                snprintf(problem, PROBLEM_SIZE, "input %.12s...:%d is used twice in the same transaction",
                         input->txid, input->output_index);
                return false;
            }
        }
        if (strcmp(set->items[position].owner, tx->sender) != 0) {
            snprintf(problem, PROBLEM_SIZE, "input %.12s...:%d belongs to %s, not %s", input->txid,
                     input->output_index, set->items[position].owner, tx->sender);
            return false;
        }
        input_total += set->items[position].amount;
    }

    if (input_total < output_total(tx) + tx->fee) {
        snprintf(problem, PROBLEM_SIZE, "inputs total %d but outputs plus fee total %d", input_total,
                 output_total(tx) + tx->fee);
        return false;
    }
    return true;
}

static bool append_utxo(UtxoSet *set, const Utxo *utxo)
{
    if (set->count == set->capacity) {
        int new_capacity = set->capacity == 0 ? INITIAL_CAPACITY : set->capacity * 2;
        Utxo *grown = realloc(set->items, (size_t)new_capacity * sizeof(Utxo));
        if (grown == NULL) {
            printf("ERROR: out of memory\n");
            return false;
        }
        set->items = grown;
        set->capacity = new_capacity;
    }
    set->items[set->count++] = *utxo;
    return true;
}

/* Removes the spent inputs and adds the new outputs, keeping the set in creation order. Validate first. */
bool utxo_apply(UtxoSet *set, const Transaction *tx)
{
    for (int i = 0; i < tx->input_count; i++) {
        int position = find_utxo(set, tx->inputs[i].txid, tx->inputs[i].output_index);
        if (position < 0) {
            return false;
        }
        memmove(&set->items[position], &set->items[position + 1],
                (size_t)(set->count - position - 1) * sizeof(Utxo));
        set->count--;
    }

    for (int i = 0; i < tx->output_count; i++) {
        Utxo utxo;
        copy_field(utxo.txid, HASH_HEX_SIZE, tx->txid);
        utxo.output_index = i;
        copy_field(utxo.owner, ID_SIZE, tx->outputs[i].owner);
        utxo.amount = tx->outputs[i].amount;
        if (!append_utxo(set, &utxo)) {
            return false;
        }
    }
    return true;
}

int utxo_balance(const UtxoSet *set, const char *owner)
{
    int balance = 0;
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].owner, owner) == 0) {
            balance += set->items[i].amount;
        }
    }
    return balance;
}

bool utxo_clone(const UtxoSet *source, UtxoSet *copy)
{
    memset(copy, 0, sizeof(*copy));
    if (source->count == 0) {
        return true;
    }
    copy->items = malloc((size_t)source->capacity * sizeof(Utxo));
    if (copy->items == NULL) {
        return false;
    }
    memcpy(copy->items, source->items, (size_t)source->count * sizeof(Utxo));
    copy->count = source->count;
    copy->capacity = source->capacity;
    return true;
}

/* Prints every unspent output, or only owner's when owner is not NULL. */
void utxo_print_set(const UtxoSet *set, const char *owner)
{
    int shown = 0;
    printf("UTXO set: %d unspent output%s\n", set->count, set->count == 1 ? "" : "s");
    printf("  %-19s %-6s %-15s %s\n", "txid", "output", "owner", "amount");
    for (int i = 0; i < set->count; i++) {
        const Utxo *utxo = &set->items[i];
        if (owner == NULL || strcmp(utxo->owner, owner) == 0) {
            printf("  %.16s... %-6d %-15s %d\n", utxo->txid, utxo->output_index, utxo->owner, utxo->amount);
            shown++;
        }
    }
    if (shown == 0) {
        printf("  (none)\n");
    }
}

void utxo_set_free(UtxoSet *set)
{
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}
