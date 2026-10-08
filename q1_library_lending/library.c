#include "library.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LIBRARY_NAME "ALU Library"

/*
 * Keys from an earlier run are reused when chain.dat was removed but the
 * keys were kept; otherwise a new librarian key pair is created.
 */
static EVP_PKEY *obtain_librarian_key(void)
{
    if (key_files_exist(LIBRARIAN_ID)) {
        printf("Found existing librarian keys, unlock them to sign the genesis block.\n");
        return load_identity_private_key(LIBRARIAN_ID);
    }
    printf("No librarian keys found.\n");
    return create_identity_keys(LIBRARIAN_ID);
}

/*
 * The genesis block holds one record: the librarian registering their own
 * public key. Every later signature is verified against keys that trace back
 * to this record.
 */
static bool create_genesis_block(LibrarySystem *library)
{
    printf("No %s found, creating the genesis block.\n", CHAIN_FILE);
    EVP_PKEY *librarian_key = obtain_librarian_key();
    if (librarian_key == NULL) {
        return false;
    }

    Block genesis;
    memset(&genesis, 0, sizeof(genesis));
    Record *registration = &genesis.records[0];
    registration->action = ACTION_REGISTERED;
    copy_field(registration->signer_id, sizeof(registration->signer_id), LIBRARIAN_ID);
    copy_field(registration->member_id, sizeof(registration->member_id), LIBRARIAN_ID);
    copy_field(registration->member_name, sizeof(registration->member_name), LIBRARY_NAME);
    registration->timestamp = time(NULL);

    bool created = public_key_to_hex(librarian_key, registration->public_key_hex)
                   && sign_record(registration, librarian_key);
    if (created) {
        genesis.record_count = 1;
        prepare_block(&genesis, &library->chain, library->difficulty, LIBRARIAN_ID);
        unsigned long attempts = 0;
        search_nonce(&genesis, ULONG_MAX, &attempts);
        printf("Genesis block mined at difficulty %d in %lu attempts: %s\n", genesis.difficulty, attempts,
               genesis.hash);
        created = block_list_append(&library->chain, &genesis) && save_chain(CHAIN_FILE, &library->chain);
    }

    if (created && validate_chain(&library->chain, &library->identities, false)) {
        find_identity(&library->identities, LIBRARIAN_ID)->private_key = librarian_key;
        return true;
    }
    EVP_PKEY_free(librarian_key);
    return false;
}

/* Warns when keys/LIBRARIAN_public.pem is not the key the genesis block registered. */
static void compare_librarian_key_file(const LibrarySystem *library)
{
    const Identity *librarian = find_identity(&library->identities, LIBRARIAN_ID);
    EVP_PKEY *file_key = load_identity_public_key(LIBRARIAN_ID);
    if (librarian != NULL && file_key != NULL && EVP_PKEY_eq(file_key, librarian->public_key) != 1) {
        printf("WARNING: %s/%s_public.pem differs from the librarian key in the genesis block\n", KEYS_DIR,
               LIBRARIAN_ID);
    }
    EVP_PKEY_free(file_key);
}

static bool load_and_validate_chain(LibrarySystem *library)
{
    if (!load_chain(CHAIN_FILE, &library->chain)) {
        return false;
    }

    library->chain_is_valid = validate_chain(&library->chain, &library->identities, false);
    if (library->chain_is_valid) {
        printf("Loaded %d block%s from %s, chain is valid.\n", library->chain.count,
               library->chain.count == 1 ? "" : "s", CHAIN_FILE);
        compare_librarian_key_file(library);
        return true;
    }

    printf("WARNING: %s failed validation:\n", CHAIN_FILE);
    validate_chain(&library->chain, NULL, true);
    printf("Only viewing and validation commands are available until the chain is restored.\n");
    return true;
}

bool library_open(LibrarySystem *library)
{
    library->difficulty = DEFAULT_DIFFICULTY;

    if (!load_registry(&library->registry, BOOKS_FILE, MEMBERS_FILE, COPIES_FILE)) {
        return false;
    }
    printf("Loaded %d books, %d copies and %d members.\n", library->registry.book_count,
           library->registry.copy_count, library->registry.member_count);

    if (access(CHAIN_FILE, F_OK) == 0) {
        return load_and_validate_chain(library);
    }
    library->chain_is_valid = create_genesis_block(library);
    return library->chain_is_valid;
}

void library_close(LibrarySystem *library)
{
    keyring_free(&library->identities);
    block_list_free(&library->chain);
}

bool require_writable_chain(const LibrarySystem *library)
{
    if (!library->chain_is_valid) {
        printf("ERROR: the chain failed validation, so nothing new can be recorded\n");
        return false;
    }
    return true;
}

bool require_librarian(LibrarySystem *library)
{
    return require_writable_chain(library) && unlock_identity(&library->identities, LIBRARIAN_ID);
}
