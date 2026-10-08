#ifndef TRANSACTION_H
#define TRANSACTION_H

#include "crypto.h"
#include "record.h"
#include "registry.h"

#include <stdbool.h>

#define MAX_TX_INPUTS 32
#define MAX_TX_OUTPUTS 8
#define TRANSACTION_FEE 1
#define NO_NONCE (-1)
#define REWARD_ON_TIME 10
#define REWARD_LATE 5
#define BLOCK_REWARD 50
#define LIBRARY_ACCOUNT "LIBRARY"
#define COINBASE_ACCOUNT "COINBASE"
#define CLOUD_PROVIDER_ACCOUNT "CLOUD-PROVIDER"
#define POOL_OPERATOR_ACCOUNT "POOL-OPERATOR"

typedef enum {
    TX_REWARD,
    TX_MINING,
    TX_TRANSFER,
    TX_FINE_PAYMENT
} TransactionType;

typedef struct {
    char owner[ID_SIZE];
    int amount;
} TxOutput;

/* A previous transaction's output being spent, used only by the UTXO model. */
typedef struct {
    char txid[HASH_HEX_SIZE];
    int output_index;
} TxInput;

/*
 * One movement of tokens. Rewards (from LIBRARY) and mining payouts (from
 * COINBASE) create coins, so they have no inputs and no nonce. Transfers and
 * fine payments come from a signed record: their txid is the record_id and
 * their nonce is the record's. The UTXO model fills in inputs and may add a
 * change output; the account model ignores both.
 */
typedef struct {
    char txid[HASH_HEX_SIZE];
    TransactionType type;
    char sender[ID_SIZE];
    int nonce;
    int fee;
    int block_index;
    TxInput inputs[MAX_TX_INPUTS];
    int input_count;
    TxOutput outputs[MAX_TX_OUTPUTS];
    int output_count;
} Transaction;

const char *transaction_type_name(TransactionType type);
bool is_minting_transaction(const Transaction *tx);
int output_total(const Transaction *tx);

int reward_for_return(const Record *record);
bool build_reward_transaction(const Record *records, int record_count, int block_index, Transaction *tx,
                              int *token_reward);
void build_mining_transaction(const TxOutput *payouts, int payout_count, int block_index, Transaction *tx);
void build_payment_transaction(const Record *record, int block_index, Transaction *tx);

#endif
