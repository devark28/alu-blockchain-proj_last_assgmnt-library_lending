#include "cli.h"
#include "input.h"
#include "library.h"

#include <stdio.h>
#include <stdlib.h>

static LibrarySystem library;

/*
 * With a command after the program name, that one command runs and the
 * program exits, so it can be scripted like git. Without one, an interactive
 * shell starts.
 */
int main(int argc, char *argv[])
{
    watch_for_interrupt();

    if (!library_open(&library)) {
        return EXIT_FAILURE;
    }

    bool succeeded = true;
    if (argc > 1) {
        succeeded = execute_command(&library, argc - 1, argv + 1);
    } else {
        run_shell(&library);
        if (was_interrupted()) {
            printf("Interrupted, closing cleanly\n");
        }
    }

    library_close(&library);
    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
