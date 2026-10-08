#ifndef COMMANDS_H
#define COMMANDS_H

#include "library.h"

#include <stdbool.h>

/* Each handler receives only the arguments after the command name, already counted by the dispatcher. */

bool command_view_member(LibrarySystem *library, int arg_count, char *args[]);
bool command_register_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_view_book(LibrarySystem *library, int arg_count, char *args[]);
bool command_add_copy(LibrarySystem *library, int arg_count, char *args[]);
bool command_view_copy(LibrarySystem *library, int arg_count, char *args[]);

bool command_chain_view(LibrarySystem *library, int arg_count, char *args[]);
bool command_validate_chain(LibrarySystem *library, int arg_count, char *args[]);
bool command_logout(LibrarySystem *library, int arg_count, char *args[]);

#endif
