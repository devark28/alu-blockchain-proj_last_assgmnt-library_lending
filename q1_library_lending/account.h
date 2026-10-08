#ifndef ACCOUNT_H
#define ACCOUNT_H

#include "keyring.h"
#include "transaction.h"

#include <stdbool.h>

typedef struct HistoryNode {
    Transaction transaction;
    struct HistoryNode *next;
} HistoryNode;

/*
 * The account's nonce is the signer nonce in its identity (see keyring.h),
 * because every request a member signs, lending or payment, uses the same
 * counter.
 */
typedef struct {
    char id[ID_SIZE];
    int balance;
    HistoryNode *history_head;
    HistoryNode *history_tail;
} Account;

typedef struct {
    Account *items;
    int count;
    int capacity;
} AccountTable;

const Account *find_account(const AccountTable *table, const char *id);
bool account_validate(const AccountTable *table, const Transaction *tx, char problem[PROBLEM_SIZE]);
bool account_apply(AccountTable *table, const Transaction *tx);
bool account_table_clone(const AccountTable *source, AccountTable *copy);
void account_print_table(const AccountTable *table, const Keyring *identities);
void account_print_history(const Account *account, const Keyring *identities);
void account_table_free(AccountTable *table);

#endif
