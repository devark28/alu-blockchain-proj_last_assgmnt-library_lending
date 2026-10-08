#include "account.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

static int account_index(const AccountTable *table, const char *id)
{
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

const Account *find_account(const AccountTable *table, const char *id)
{
    int index = account_index(table, id);
    return index >= 0 ? &table->items[index] : NULL;
}

static Account *find_or_create_account(AccountTable *table, const char *id)
{
    int index = account_index(table, id);
    if (index >= 0) {
        return &table->items[index];
    }
    if (table->count == table->capacity) {
        int new_capacity = table->capacity == 0 ? INITIAL_CAPACITY : table->capacity * 2;
        Account *grown = realloc(table->items, (size_t)new_capacity * sizeof(Account));
        if (grown == NULL) {
            printf("ERROR: out of memory\n");
            return NULL;
        }
        table->items = grown;
        table->capacity = new_capacity;
    }

    Account *account = &table->items[table->count++];
    memset(account, 0, sizeof(*account));
    copy_field(account->id, ID_SIZE, id);
    return account;
}

/*
 * Minted transactions create coins. A payment must come from an account
 * whose balance covers the amount plus the fee. The nonce was already
 * checked against the signer's identity before this point.
 */
bool account_validate(const AccountTable *table, const Transaction *tx, char problem[PROBLEM_SIZE])
{
    if (output_total(tx) <= 0) {
        snprintf(problem, PROBLEM_SIZE, "amount must be positive");
        return false;
    }
    if (is_minting_transaction(tx)) {
        return true;
    }

    const Account *sender = find_account(table, tx->sender);
    int balance = sender != NULL ? sender->balance : 0;
    int needed = output_total(tx) + tx->fee;
    if (balance < needed) {
        snprintf(problem, PROBLEM_SIZE, "insufficient balance: %s has %d coins but needs %d (%d + %d fee)",
                 tx->sender, balance, needed, output_total(tx), tx->fee);
        return false;
    }
    return true;
}

static bool add_history(Account *account, const Transaction *tx)
{
    HistoryNode *node = malloc(sizeof(HistoryNode));
    if (node == NULL) {
        printf("ERROR: out of memory\n");
        return false;
    }
    node->transaction = *tx;
    node->next = NULL;

    if (account->history_tail == NULL) {
        account->history_head = node;
    } else {
        account->history_tail->next = node;
    }
    account->history_tail = node;
    return true;
}

/* Debits the sender the amount plus fee and credits every output. Call account_validate first. */
bool account_apply(AccountTable *table, const Transaction *tx)
{
    if (!is_minting_transaction(tx)) {
        Account *sender = find_or_create_account(table, tx->sender);
        if (sender == NULL || !add_history(sender, tx)) {
            return false;
        }
        sender->balance -= output_total(tx) + tx->fee;
    }

    for (int i = 0; i < tx->output_count; i++) {
        Account *recipient = find_or_create_account(table, tx->outputs[i].owner);
        if (recipient == NULL) {
            return false;
        }
        recipient->balance += tx->outputs[i].amount;
        bool already_listed = recipient->history_tail != NULL
                              && strcmp(recipient->history_tail->transaction.txid, tx->txid) == 0;
        if (!already_listed && !add_history(recipient, tx)) {
            return false;
        }
    }
    return true;
}

static void free_history(HistoryNode *node)
{
    while (node != NULL) {
        HistoryNode *next = node->next;
        free(node);
        node = next;
    }
}

bool account_table_clone(const AccountTable *source, AccountTable *copy)
{
    memset(copy, 0, sizeof(*copy));
    for (int i = 0; i < source->count; i++) {
        const Account *original = &source->items[i];
        Account *account = find_or_create_account(copy, original->id);
        if (account == NULL) {
            account_table_free(copy);
            return false;
        }
        account->balance = original->balance;
        for (const HistoryNode *node = original->history_head; node != NULL; node = node->next) {
            if (!add_history(account, &node->transaction)) {
                account_table_free(copy);
                return false;
            }
        }
    }
    return true;
}

static int next_nonce(const Keyring *identities, const char *id)
{
    const Identity *identity = find_identity(identities, id);
    return identity != NULL ? identity->next_nonce : 0;
}

void account_print_table(const AccountTable *table, const Keyring *identities)
{
    printf("Accounts: %d\n", table->count);
    printf("  %-15s %8s %10s\n", "account", "balance", "next nonce");
    for (int i = 0; i < table->count; i++) {
        const Account *account = &table->items[i];
        printf("  %-15s %8d %10d\n", account->id, account->balance, next_nonce(identities, account->id));
    }
}

static void describe_recipients(const Transaction *tx, char *buffer, size_t size)
{
    size_t used = 0;
    buffer[0] = '\0';
    for (int i = 0; i < tx->output_count && used < size; i++) {
        used += (size_t)snprintf(buffer + used, size - used, "%s%s %d", i > 0 ? ", " : "", tx->outputs[i].owner,
                                 tx->outputs[i].amount);
    }
}

void account_print_history(const Account *account, const Keyring *identities)
{
    printf("Transaction history for %s: balance %d, next nonce %d\n", account->id, account->balance,
           next_nonce(identities, account->id));
    if (account->history_head == NULL) {
        printf("  (no transactions)\n");
        return;
    }

    printf("  %-5s %-8s %-10s %4s %5s  %-14s %s\n", "block", "type", "sender", "fee", "nonce", "txid", "recipients");
    for (const HistoryNode *node = account->history_head; node != NULL; node = node->next) {
        const Transaction *tx = &node->transaction;
        char nonce[12] = "-";
        char recipients[200];
        if (tx->nonce != NO_NONCE) {
            snprintf(nonce, sizeof(nonce), "%d", tx->nonce);
        }
        describe_recipients(tx, recipients, sizeof(recipients));
        printf("  %-5d %-8s %-10s %4d %5s  %.12s.. %s\n", tx->block_index, transaction_type_name(tx->type),
               tx->sender, tx->fee, nonce, tx->txid, recipients);
    }
}

void account_table_free(AccountTable *table)
{
    for (int i = 0; i < table->count; i++) {
        free_history(table->items[i].history_head);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}
