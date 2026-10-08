#ifndef LIBRARY_H
#define LIBRARY_H

#include "blockchain.h"
#include "keyring.h"
#include "pool.h"
#include "state.h"

#include <stdbool.h>

#define BOOKS_FILE "books.txt"
#define MEMBERS_FILE "members.txt"
#define COPIES_FILE "copies.txt"
#define CHAIN_FILE "chain.dat"
#define POOL_FILE "pool.dat"
#define AUTOMATIC_NONCE (-1)

/*
 * Everything the program holds in memory. state is rebuilt from the chain
 * on every start. chain_is_valid stays false after a failed validation, so
 * that only read-only commands can run.
 */
typedef struct {
    LibraryState state;
    BlockList chain;
    Pool pool;
    KeyStore keys;
    int difficulty;
    bool chain_is_valid;
} LibrarySystem;

bool library_open(LibrarySystem *library, LedgerModel model, int difficulty);
void library_close(LibrarySystem *library);

bool require_writable_chain(const LibrarySystem *library);
EVP_PKEY *unlock_librarian(LibrarySystem *library);
EVP_PKEY *unlock_member(LibrarySystem *library, const char *member_id);

LibraryState *build_effective_state(const LibrarySystem *library);
bool submit_request(LibrarySystem *library, Record *record, EVP_PKEY *signing_key);
bool submit_request_with_nonce(LibrarySystem *library, Record *record, EVP_PKEY *signing_key, int nonce);
bool resubmit_signed_record(LibrarySystem *library, const Record *record);
void prune_pool(LibrarySystem *library);

#endif
