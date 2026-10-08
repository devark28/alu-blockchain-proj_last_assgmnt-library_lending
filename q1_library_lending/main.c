#include "cli.h"
#include "input.h"
#include "library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LibrarySystem library;

static void print_usage(const char *program)
{
    printf("usage: %s [--model utxo|account] [--difficulty %d-%d] [command [arguments]]\n", program,
           MIN_DIFFICULTY, MAX_DIFFICULTY);
}

/* Reads the options in front of the command and returns where the command starts, or -1 on a bad option. */
static int parse_options(int argc, char *argv[], LedgerModel *model, int *difficulty)
{
    int index = 1;
    while (index < argc && strncmp(argv[index], "--", 2) == 0) {
        if (index + 1 >= argc) {
            return -1;
        }
        if (strcmp(argv[index], "--model") == 0) {
            if (!parse_model(argv[index + 1], model)) {
                return -1;
            }
        } else if (strcmp(argv[index], "--difficulty") == 0) {
            char *end;
            long value = strtol(argv[index + 1], &end, 10);
            if (*end != '\0' || value < MIN_DIFFICULTY || value > MAX_DIFFICULTY) {
                return -1;
            }
            *difficulty = (int)value;
        } else {
            return -1;
        }
        index += 2;
    }
    return index;
}

/*
 * With a command after the options, that one command runs and the program
 * exits, so it can be scripted like git. Without one, an interactive shell
 * starts.
 */
int main(int argc, char *argv[])
{
    LedgerModel model = MODEL_UTXO;
    int difficulty = DEFAULT_DIFFICULTY;
    int command_start = parse_options(argc, argv, &model, &difficulty);
    if (command_start < 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    watch_for_interrupt();
    if (!library_open(&library, model, difficulty)) {
        library_close(&library);
        return EXIT_FAILURE;
    }

    bool succeeded = true;
    if (command_start < argc) {
        succeeded = execute_command(&library, argc - command_start, argv + command_start);
    } else {
        run_shell(&library);
        if (was_interrupted()) {
            printf("Interrupted, closing cleanly\n");
        }
    }

    library_close(&library);
    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
