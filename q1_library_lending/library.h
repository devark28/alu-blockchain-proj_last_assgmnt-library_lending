#ifndef LIBRARY_H
#define LIBRARY_H

#include "registry.h"

#include <stdbool.h>

#define BOOKS_FILE "books.txt"
#define MEMBERS_FILE "members.txt"
#define COPIES_FILE "copies.txt"

typedef struct {
    Registry registry;
} LibrarySystem;

bool library_open(LibrarySystem *library);
void library_close(LibrarySystem *library);

#endif
