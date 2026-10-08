#include "transaction.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

#define TX_PAYLOAD_SIZE 512

static const char *TYPE_NAMES[] = {"REWARD", "MINING", "TRANSFER", "FINE"};

const char *transaction_type_name(TransactionType type)
{
    return TYPE_NAMES[type];
}

bool is_minting_transaction(const Transaction *tx)
{
    return tx->type == TX_REWARD || tx->type == TX_MINING;
}

int output_total(const Transaction *tx)
{
    int total = 0;
    for (int i = 0; i < tx->output_count; i++) {
        total += tx->outputs[i].amount;
    }
    return total;
}

static void add_output(Transaction *tx, const char *owner, int amount)
{
    TxOutput *output = &tx->outputs[tx->output_count++];
    copy_field(output->owner, ID_SIZE, owner);
    output->amount = amount;
}

/* A minted transaction has no signed record, so its ID is the hash of its contents and block. */
static void compute_minted_txid(Transaction *tx)
{
    char payload[TX_PAYLOAD_SIZE];
    int used = snprintf(payload, sizeof(payload), "%s|%s|%d|%d", transaction_type_name(tx->type), tx->sender,
                        tx->block_index, tx->fee);
    for (int i = 0; i < tx->output_count && used < (int)sizeof(payload); i++) {
        used += snprintf(payload + used, sizeof(payload) - (size_t)used, "|%s:%d", tx->outputs[i].owner,
                         tx->outputs[i].amount);
    }
    sha256_hex(payload, tx->txid);
}

static void start_transaction(Transaction *tx, TransactionType type, const char *sender, int block_index)
{
    memset(tx, 0, sizeof(*tx));
    tx->type = type;
    tx->nonce = NO_NONCE;
    tx->block_index = block_index;
    copy_field(tx->sender, ID_SIZE, sender);
}

/* A confirmed return earns REWARD_ON_TIME, or REWARD_LATE when it carried a fine. Other records earn nothing. */
int reward_for_return(const Record *record)
{
    if (record->action != ACTION_RETURNED) {
        return 0;
    }
    return record->fine_amount == 0 ? REWARD_ON_TIME : REWARD_LATE;
}

/*
 * The block's reward transaction pays every member who returned a book in
 * it, minus TRANSACTION_FEE each. It is built only from the block's records,
 * so anyone holding the chain can rebuild it and compare its txid with the
 * block's transaction_id. Returns false when the block has no returns.
 */
bool build_reward_transaction(const Record *records, int record_count, int block_index, Transaction *tx,
                              int *token_reward)
{
    start_transaction(tx, TX_REWARD, LIBRARY_ACCOUNT, block_index);
    *token_reward = 0;
    for (int i = 0; i < record_count && tx->output_count < MAX_TX_OUTPUTS; i++) {
        int reward = reward_for_return(&records[i]);
        if (reward > 0) {
            add_output(tx, records[i].member_id, reward - TRANSACTION_FEE);
            tx->fee += TRANSACTION_FEE;
            *token_reward += reward;
        }
    }
    if (tx->output_count == 0) {
        return false;
    }
    compute_minted_txid(tx);
    return true;
}

void build_mining_transaction(const TxOutput *payouts, int payout_count, int block_index, Transaction *tx)
{
    start_transaction(tx, TX_MINING, COINBASE_ACCOUNT, block_index);
    for (int i = 0; i < payout_count && i < MAX_TX_OUTPUTS; i++) {
        add_output(tx, payouts[i].owner, payouts[i].amount);
    }
    compute_minted_txid(tx);
}

/* A TRANSFER or FINE_PAID record becomes a payment from the member who signed it. */
void build_payment_transaction(const Record *record, int block_index, Transaction *tx)
{
    TransactionType type = record->action == ACTION_FINE_PAID ? TX_FINE_PAYMENT : TX_TRANSFER;
    start_transaction(tx, type, record->member_id, block_index);
    copy_field(tx->txid, HASH_HEX_SIZE, record->record_id);
    tx->nonce = record->nonce;
    tx->fee = TRANSACTION_FEE;
    add_output(tx, record->recipient_id, record->amount);
}
