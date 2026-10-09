#include "cli.h"
#include "commands.h"
#include "input.h"

#include <stdio.h>
#include <string.h>

#define COMMAND_LINE_SIZE 512
#define MAX_ARGUMENTS 8
#define UNCLOSED_QUOTE (-1)
#define TOO_MANY_ARGUMENTS (-2)

typedef bool (*CommandHandler)(LibrarySystem *library, int arg_count, char *args[]);

typedef struct {
    const char *group;
    const char *name;
    const char *arguments;
    int min_args;
    int max_args;
    CommandHandler run;
    const char *summary;
} Command;

static const Command COMMANDS[] = {
    {"Membership", "register_member", "<member_id> \"<full_name>\" <course_code>", 3, 3, command_register_member,
     "add a member and put their public key on the chain"},
    {"Membership", "view_member", "[member_id]", 0, 1, command_view_member, "list members, or show one member"},
    {"Membership", "member_nonce", "<member_id>", 1, 1, command_member_nonce, "show the nonce a member signs with"},
    {"Books and copies", "register_book", "<book_id> \"<title>\" \"<author>\"", 3, 3, command_register_book,
     "add a title to books.txt"},
    {"Books and copies", "view_book", "[book_id]", 0, 1, command_view_book, "list books, or show one book and its copies"},
    {"Books and copies", "add_copy", "<book_id> <NEW|GOOD|WORN>", 2, 2, command_add_copy,
     "add a physical copy to copies.txt"},
    {"Books and copies", "view_copy", "<copy_id>", 1, 1, command_view_copy, "show one copy's state"},
    {"Books and copies", "mark_lost", "<copy_id> <reason_code>", 2, 2, command_mark_lost,
     "librarian override: the copy is lost"},
    {"Lending", "borrow_book", "<book_id> <member_id>", 2, 2, command_borrow_book, "borrow an available copy"},
    {"Lending", "return_book", "<copy_id> <NEW|GOOD|WORN> [reason_code]", 2, 3, command_return_book,
     "return a copy and record its condition"},
    {"Lending", "renew_book", "<copy_id>", 1, 1, command_renew_book, "extend the due date"},
    {"Lending", "reserve_book", "<book_id> <member_id>", 2, 2, command_reserve_book,
     "join the queue for a title with no copy available"},
    {"Lending", "reservation_status", "[book_id]", 0, 1, command_reservation_status,
     "show reservation queues and held copies"},
    {"Fines and overdue", "check_overdue", "", 0, 0, command_check_overdue, "record overdue loans and list them"},
    {"Fines and overdue", "view_fines", "[member_id]", 0, 1, command_view_fines, "fines owed and still accruing"},
    {"Fines and overdue", "settle_fine", "<member_id>", 1, 1, command_settle_fine, "pay the fines owed in tokens"},
    {"Tokens and ledger", "token_transfer", "<from_id> <to_id> <amount>", 3, 3, command_token_transfer,
     "send tokens in the session's model"},
    {"Tokens and ledger", "token_balance", "<member_id>", 1, 1, command_token_balance, "a member's balance"},
    {"Tokens and ledger", "utxo_view", "[member_id]", 0, 1, command_utxo_view, "UTXO model: unspent outputs"},
    {"Tokens and ledger", "account_balance", "[member_id]", 0, 1, command_account_balance,
     "account model: balances and nonces"},
    {"Tokens and ledger", "account_transfer", "<from_id> <to_id> <amount> <nonce>", 4, 4, command_account_transfer,
     "account model: send tokens with an explicit nonce"},
    {"Tokens and ledger", "account_nonce", "<member_id>", 1, 1, command_member_nonce, "the nonce a member signs with"},
    {"Tokens and ledger", "transaction_history", "<member_id>", 1, 1, command_transaction_history,
     "account model: a member's transaction list"},
    {"Tokens and ledger", "double_spend_test", "", 0, 0, command_double_spend_test,
     "UTXO model: spend the last payment's inputs again"},
    {"Blockchain and mining", "pool_view", "", 0, 0, command_pool_view, "pending requests in mining order"},
    {"Blockchain and mining", "mine_solo", "<member_id>", 1, 1, command_mine_solo,
     "one miner confirms every pending request"},
    {"Blockchain and mining", "mine_pool", "<member_id>", 1, 1, command_mine_pool,
     "the member and 3 peers mine and share rewards"},
    {"Blockchain and mining", "mine_cloud", "<rounds> <member_id>", 2, 2, command_mine_cloud,
     "rent mining power for 1 to 5 rounds"},
    {"Blockchain and mining", "chain_save", "", 0, 0, command_chain_save, "write the chain and every snapshot to disk"},
    {"Blockchain and mining", "chain_load", "", 0, 0, command_chain_load,
     "reload everything from disk and validate it"},
    {"Blockchain and mining", "difficulty_status", "", 0, 0, command_difficulty_status,
     "current difficulty and recent attempt counts"},
    {"Blockchain and mining", "set_difficulty", "<1-4>", 1, 1, command_set_difficulty,
     "librarian: change the mining difficulty"},
    {"Blockchain and mining", "chain_view", "[index]", 0, 1, command_chain_view, "list blocks, or show one block in full"},
    {"Blockchain and mining", "validate_chain", "", 0, 0, command_validate_chain, "check every block and record"},
    {"Fraud and audit", "fraud_review", "", 0, 0, command_fraud_review, "requests held as SUSPICIOUS and why"},
    {"Fraud and audit", "approve_suspicious", "<id>", 1, 1, command_approve_suspicious,
     "librarian: release a flagged request for mining"},
    {"Fraud and audit", "reject_suspicious", "<id>", 1, 1, command_reject_suspicious,
     "librarian: discard a flagged request"},
    {"Fraud and audit", "replay_request", "<id>", 1, 1, command_replay_request,
     "test: submit a copy of an already signed request"},
    {"Fraud and audit", "lending_history", "", 0, 0, command_lending_history, "every confirmed lending record"},
    {"Fraud and audit", "member_history", "<member_id>", 1, 1, command_member_history,
     "every confirmed record involving a member"},
    {"Fraud and audit", "book_history", "<book_id>", 1, 1, command_book_history,
     "every confirmed record for a title's copies"},
    {"Fraud and audit", "tamper_demo", "[block]", 0, 1, command_tamper_demo,
     "attack a copy of the chain and show what catches it"},
    {"Keys", "logout", "", 0, 0, command_logout, "lock every unlocked private key"},
};

static const int COMMAND_COUNT = (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0]));

static const Command *find_command(const char *name)
{
    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(COMMANDS[i].name, name) == 0) {
            return &COMMANDS[i];
        }
    }
    return NULL;
}

static void print_usage(const Command *command)
{
    printf("usage: %s %s\n", command->name, command->arguments);
}

static void print_help(void)
{
    const char *current_group = "";
    for (int i = 0; i < COMMAND_COUNT; i++) {
        const Command *command = &COMMANDS[i];
        if (strcmp(command->group, current_group) != 0) {
            current_group = command->group;
            printf("\n%s\n", current_group);
        }
        char signature[96];
        snprintf(signature, sizeof(signature), "%s %s", command->name, command->arguments);
        printf("  %-56s %s\n", signature, command->summary);
    }
    printf("\nShell\n");
    printf("  %-56s %s\n", "help", "show this list");
    printf("  %-56s %s\n", "exit", "leave the shell");
    printf("\nWrap values that contain spaces in double quotes.\n");
}

/*
 * Splits line in place into arguments separated by spaces. Text inside
 * double quotes is one argument, so titles and names can contain spaces.
 */
static int split_arguments(char *line, char *args[], int max_args)
{
    int count = 0;
    char *cursor = line;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count == max_args) {
            return TOO_MANY_ARGUMENTS;
        }

        if (*cursor == '"') {
            char *closing = strchr(cursor + 1, '"');
            if (closing == NULL) {
                return UNCLOSED_QUOTE;
            }
            args[count++] = cursor + 1;
            *closing = '\0';
            cursor = closing + 1;
            if (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
                return UNCLOSED_QUOTE;
            }
        } else {
            args[count++] = cursor;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
                cursor++;
            }
            if (*cursor != '\0') {
                *cursor++ = '\0';
            }
        }
    }
    return count;
}

bool execute_command(LibrarySystem *library, int arg_count, char *args[])
{
    if (strcmp(args[0], "help") == 0) {
        print_help();
        return true;
    }

    const Command *command = find_command(args[0]);
    if (command == NULL) {
        printf("ERROR: unknown command '%s', type help for the list\n", args[0]);
        return false;
    }

    int given = arg_count - 1;
    if (given < command->min_args || given > command->max_args) {
        print_usage(command);
        return false;
    }
    if (command->run != command_check_overdue) {
        scan_for_overdue_loans(library);
    }
    return command->run(library, given, args + 1);
}

void run_shell(LibrarySystem *library)
{
    char line[COMMAND_LINE_SIZE];
    printf("Type help for the list of commands.\n");

    while (true) {
        printf("\nlibrary> ");
        fflush(stdout);
        if (!read_line(line, sizeof(line))) {
            printf("\n");
            break;
        }

        char *args[MAX_ARGUMENTS];
        int arg_count = split_arguments(line, args, MAX_ARGUMENTS);
        if (arg_count == UNCLOSED_QUOTE) {
            printf("ERROR: a quote is not closed, or a closing quote is not followed by a space\n");
            continue;
        }
        if (arg_count == TOO_MANY_ARGUMENTS) {
            printf("ERROR: too many arguments, at most %d are accepted\n", MAX_ARGUMENTS - 1);
            continue;
        }
        if (arg_count == 0) {
            continue;
        }
        if (strcmp(args[0], "exit") == 0) {
            break;
        }
        execute_command(library, arg_count, args);
    }
}
