#ifndef CLI_H
#define CLI_H

#include "library.h"

#include <stdbool.h>

bool execute_command(LibrarySystem *library, int arg_count, char *args[]);
void run_shell(LibrarySystem *library);

#endif
