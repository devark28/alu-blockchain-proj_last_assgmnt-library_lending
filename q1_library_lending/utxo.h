#ifndef UTXO_H
#define UTXO_H

#include "transaction.h"

#include <stdbool.h>

/* An unspent output: amount coins owned by owner, created by output_index of txid. */
typedef struct {
    char txid[HASH_HEX_SIZE];
    int output_index;
    char owner[ID_SIZE];
    int amount;
} Utxo;

typedef struct {
    Utxo *items;
    int count;
    int capacity;
} UtxoSet;

bool utxo_select_inputs(const UtxoSet *set, Transaction *tx, char problem[PROBLEM_SIZE]);
bool utxo_validate(const UtxoSet *set, const Transaction *tx, char problem[PROBLEM_SIZE]);
bool utxo_apply(UtxoSet *set, const Transaction *tx);
int utxo_balance(const UtxoSet *set, const char *owner);
bool utxo_clone(const UtxoSet *source, UtxoSet *copy);
void utxo_print_set(const UtxoSet *set, const char *owner);
void utxo_set_free(UtxoSet *set);

#endif
