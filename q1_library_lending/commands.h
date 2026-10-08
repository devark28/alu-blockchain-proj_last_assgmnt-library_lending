#ifndef COMMANDS_H
#define COMMANDS_H

#include "library.h"

#include <stdbool.h>

/* Each handler receives only the arguments after the command name, already counted by the dispatcher. */

const Book *require_book(const Registry *registry, const char *book_id);
const Member *require_member(const Registry *registry, const char *member_id);
Copy *require_copy(const Registry *registry, const char *copy_id);
void scan_for_overdue_loans(LibrarySystem *library);

bool command_view_member(LibrarySystem *library, int arg_count, char *args[]);
bool command_register_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_view_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_add_copy(LibrarySystem *library, int arg_count, char *args[]);
bool command_view_copy(LibrarySystem *library, int arg_count, char *args[]);

bool command_chain_view(LibrarySystem *library, int arg_count, char *args[]);
bool command_validate_chain(LibrarySystem *library, int arg_count, char *args[]);
bool command_logout(LibrarySystem *library, int arg_count, char *args[]);

bool command_register_member(LibrarySystem *library, int arg_count, char *args[]);
bool command_member_nonce(LibrarySystem *library, int arg_count, char *args[]);
bool command_borrow_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_return_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_renew_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_reserve_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_reservation_status(LibrarySystem *library, int arg_count, char *args[]);
bool command_mark_lost(LibrarySystem *library, int arg_count, char *args[]);
bool command_check_overdue(LibrarySystem *library, int arg_count, char *args[]);
bool command_view_fines(LibrarySystem *library, int arg_count, char *args[]);
bool command_pool_view(LibrarySystem *library, int arg_count, char *args[]);
bool command_mine_solo(LibrarySystem *library, int arg_count, char *args[]);
bool command_fraud_review(LibrarySystem *library, int arg_count, char *args[]);
bool command_approve_suspicious(LibrarySystem *library, int arg_count, char *args[]);
bool command_reject_suspicious(LibrarySystem *library, int arg_count, char *args[]);
bool command_replay_request(LibrarySystem *library, int arg_count, char *args[]);
bool command_token_transfer(LibrarySystem *library, int arg_count, char *args[]);
bool command_token_balance(LibrarySystem *library, int arg_count, char *args[]);
bool command_utxo_view(LibrarySystem *library, int arg_count, char *args[]);
bool command_account_balance(LibrarySystem *library, int arg_count, char *args[]);
bool command_account_transfer(LibrarySystem *library, int arg_count, char *args[]);
bool command_transaction_history(LibrarySystem *library, int arg_count, char *args[]);
bool command_settle_fine(LibrarySystem *library, int arg_count, char *args[]);
bool command_double_spend_test(LibrarySystem *library, int arg_count, char *args[]);
bool command_mine_pool(LibrarySystem *library, int arg_count, char *args[]);
bool command_mine_cloud(LibrarySystem *library, int arg_count, char *args[]);
bool command_difficulty_status(LibrarySystem *library, int arg_count, char *args[]);
bool command_set_difficulty(LibrarySystem *library, int arg_count, char *args[]);
bool command_lending_history(LibrarySystem *library, int arg_count, char *args[]);
bool command_member_history(LibrarySystem *library, int arg_count, char *args[]);
bool command_book_history(LibrarySystem *library, int arg_count, char *args[]);
bool command_chain_save(LibrarySystem *library, int arg_count, char *args[]);
bool command_chain_load(LibrarySystem *library, int arg_count, char *args[]);
bool command_tamper_demo(LibrarySystem *library, int arg_count, char *args[]);

#endif
