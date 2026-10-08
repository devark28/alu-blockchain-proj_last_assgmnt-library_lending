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
    {"Membership", "view_member", "[member_id]", 0, 1, command_view_member, "list members, or show one member"},
    {"Books and copies", "register_book", "<book_id> \"<title>\" \"<author>\"", 3, 3, command_register_book,
     "add a title to books.txt"},
    {"Books and copies", "view_book", "[book_id]", 0, 1, command_view_book, "list books, or show one book and its copies"},
    {"Books and copies", "add_copy", "<book_id> <NEW|GOOD|WORN>", 2, 2, command_add_copy,
     "add a physical copy to copies.txt"},
    {"Books and copies", "view_copy", "<copy_id>", 1, 1, command_view_copy, "show one copy's state"},
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
        printf("  %-48s %s\n", signature, command->summary);
    }
    printf("\nShell\n");
    printf("  %-48s %s\n", "help", "show this list");
    printf("  %-48s %s\n", "exit", "leave the shell");
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
