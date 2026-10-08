#ifndef LIBRARY_H
#define LIBRARY_H

#include "blockchain.h"
#include "keyring.h"
#include "registry.h"

#include <stdbool.h>

#define BOOKS_FILE "books.txt"
#define MEMBERS_FILE "members.txt"
#define COPIES_FILE "copies.txt"
#define CHAIN_FILE "chain.dat"

/*
 * Everything the program holds in memory. identities is rebuilt from the
 * chain on every start, and chain_is_valid stays false after a failed
 * validation so that only read-only commands can run.
 */
typedef struct {
    Registry registry;
    BlockList chain;
    Keyring identities;
    int difficulty;
    bool chain_is_valid;
} LibrarySystem;

bool library_open(LibrarySystem *library);
void library_close(LibrarySystem *library);
bool require_writable_chain(const LibrarySystem *library);
bool require_librarian(LibrarySystem *library);

#endif
