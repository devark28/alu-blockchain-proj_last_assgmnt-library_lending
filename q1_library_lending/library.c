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

    char problem[PROBLEM_SIZE];
    if (created && apply_record(&library->state, registration, problem)) {
        return keep_unlocked_key(&library->keys, LIBRARIAN_ID, librarian_key);
    }
    EVP_PKEY_free(librarian_key);
    return false;
}

/* Warns when keys/LIBRARIAN_public.pem is not the key the genesis block registered. */
static void compare_librarian_key_file(const LibrarySystem *library)
{
    const Identity *librarian = find_identity(&library->state.identities, LIBRARIAN_ID);
    EVP_PKEY *file_key = load_identity_public_key(LIBRARIAN_ID);
    if (librarian != NULL && file_key != NULL && EVP_PKEY_eq(file_key, librarian->public_key) != 1) {
        printf("WARNING: %s/%s_public.pem differs from the librarian key in the genesis block\n", KEYS_DIR,
               LIBRARIAN_ID);
    }
    EVP_PKEY_free(file_key);
}

/*
 * The chain is checked block by block, then replayed through the lending
 * rules to rebuild copy states, queues, fines and nonces. A chain that fails
 * either step is kept for viewing but nothing new can be added to it.
 */
static bool load_and_validate_chain(LibrarySystem *library)
{
    if (!load_chain(CHAIN_FILE, &library->chain)) {
        return false;
    }

    library->chain_is_valid = validate_chain(&library->chain, NULL, false);
    if (!library->chain_is_valid) {
        printf("WARNING: %s failed validation:\n", CHAIN_FILE);
        validate_chain(&library->chain, NULL, true);
    } else {
        library->chain_is_valid = replay_chain(&library->state, &library->chain);
    }

    if (!library->chain_is_valid) {
        printf("Only viewing and validation commands are available until the chain is restored.\n");
        return true;
    }
    printf("Loaded %d block%s from %s, chain is valid.\n", library->chain.count,
           library->chain.count == 1 ? "" : "s", CHAIN_FILE);
    compare_librarian_key_file(library);
    return true;
}

bool library_open(LibrarySystem *library)
{
    library->difficulty = DEFAULT_DIFFICULTY;

    if (!load_registry(&library->state.registry, BOOKS_FILE, MEMBERS_FILE, COPIES_FILE)) {
        return false;
    }
    printf("Loaded %d books, %d copies and %d members.\n", library->state.registry.book_count,
           library->state.registry.copy_count, library->state.registry.member_count);

    bool opened;
    if (access(CHAIN_FILE, F_OK) == 0) {
        opened = load_and_validate_chain(library);
    } else {
        opened = create_genesis_block(library);
        library->chain_is_valid = opened;
    }
    if (!opened) {
        return false;
    }

    if (!load_pool(POOL_FILE, &library->pool)) {
        return false;
    }
    if (library->chain_is_valid) {
        prune_pool(library);
    }
    return true;
}

void library_close(LibrarySystem *library)
{
    lock_all_keys(&library->keys);
    free_state_contents(&library->state);
    block_list_free(&library->chain);
    pool_free(&library->pool);
}

bool require_writable_chain(const LibrarySystem *library)
{
    if (!library->chain_is_valid) {
        printf("ERROR: the chain failed validation, so nothing new can be recorded\n");
        return false;
    }
    return true;
}

EVP_PKEY *unlock_librarian(LibrarySystem *library)
{
    if (!require_writable_chain(library)) {
        return NULL;
    }
    return unlock_key(&library->keys, &library->state.identities, LIBRARIAN_ID);
}

/* The confirmed state with every pending request applied in submission order. */
LibraryState *build_effective_state(const LibrarySystem *library)
{
    LibraryState *effective = clone_state(&library->state);
    if (effective == NULL) {
        return NULL;
    }
    for (int i = 0; i < library->pool.count; i++) {
        char problem[PROBLEM_SIZE];
        apply_record(effective, &library->pool.items[i].record, problem);
    }
    return effective;
}

/*
 * A member who has never signed anything has no key on the chain yet. Their
 * key pair is created (or an existing key file reused) and the librarian
 * signs a REGISTERED record that puts the public key on the chain. The
 * member's own request can follow straight away, because requests are
 * checked against the pool as well as the chain.
 */
static EVP_PKEY *register_member_identity(LibrarySystem *library, const Member *member)
{
    printf("%s has no key on the chain yet. Their key pair is created now and the librarian signs the registration.\n",
           member->member_id);
    EVP_PKEY *librarian_key = unlock_librarian(library);
    if (librarian_key == NULL) {
        return NULL;
    }
    EVP_PKEY *member_key = key_files_exist(member->member_id) ? load_identity_private_key(member->member_id)
                                                              : create_identity_keys(member->member_id);
    if (member_key == NULL) {
        return NULL;
    }

    Record registration;
    memset(&registration, 0, sizeof(registration));
    registration.action = ACTION_REGISTERED;
    registration.timestamp = time(NULL);
    copy_field(registration.signer_id, ID_SIZE, LIBRARIAN_ID);
    copy_field(registration.member_id, ID_SIZE, member->member_id);
    copy_field(registration.member_name, NAME_SIZE, member->full_name);

    if (!public_key_to_hex(member_key, registration.public_key_hex)
        || !submit_request(library, &registration, librarian_key)
        || !keep_unlocked_key(&library->keys, member->member_id, member_key)) {
        EVP_PKEY_free(member_key);
        return NULL;
    }
    return member_key;
}

EVP_PKEY *unlock_member(LibrarySystem *library, const char *member_id)
{
    if (!require_writable_chain(library)) {
        return NULL;
    }
    EVP_PKEY *key = find_unlocked_key(&library->keys, member_id);
    if (key != NULL) {
        return key;
    }

    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return NULL;
    }
    const Member *member = find_member(&library->state.registry, member_id);
    if (find_identity(&effective->identities, member_id) != NULL) {
        key = unlock_key(&library->keys, &effective->identities, member_id);
    } else if (member != NULL) {
        key = register_member_identity(library, member);
    } else {
        printf("ERROR: Member not found: %s\n", member_id);
    }
    free_state(effective);
    return key;
}

/*
 * Overdue returns and fine payments are served first. Among the rest, a
 * member collecting a copy held for them, or waiting in a queue, counts from
 * the date they reserved; everyone else from the moment they asked.
 */
static void set_priority(const LibraryState *effective, PendingRequest *request)
{
    const Record *record = &request->record;
    const Copy *copy = record->copy_id[0] != '\0' ? find_copy(&effective->registry, record->copy_id) : NULL;
    bool overdue_related = record->action == ACTION_OVERDUE || record->action == ACTION_FINE_PAID
                           || (record->action == ACTION_RETURNED && copy != NULL
                               && record->timestamp > copy->due_date);

    request->priority = overdue_related ? PRIORITY_OVERDUE : PRIORITY_NORMAL;
    request->order_time = record->timestamp;

    const ReservationNode *reservation = find_reservation(&effective->reservations, record->book_id,
                                                          record->member_id);
    if (record->action == ACTION_BORROWED && copy != NULL && copy->state == COPY_RESERVED) {
        request->order_time = copy->reserved_at;
    } else if (record->action == ACTION_BORROWED && reservation != NULL) {
        request->order_time = reservation->reservation_date;
    }
}

/*
 * Checks a new request against the chain plus everything already pending,
 * gives it the signer's next nonce, signs it and adds it to the pool. The
 * caller fills in the action, IDs and timestamp.
 */
bool submit_request(LibrarySystem *library, Record *record, EVP_PKEY *signing_key)
{
    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return false;
    }

    const Identity *signer = find_identity(&effective->identities, record->signer_id);
    if (signer == NULL) {
        printf("ERROR: %s has no registered public key\n", record->signer_id);
        free_state(effective);
        return false;
    }
    record->nonce = signer->next_nonce;

    PendingRequest request;
    memset(&request, 0, sizeof(request));
    request.record = *record;
    request.status = REQUEST_PENDING;
    set_priority(effective, &request);

    char problem[PROBLEM_SIZE];
    bool accepted = apply_record(effective, record, problem);
    free_state(effective);
    if (!accepted) {
        printf("ERROR: %s\n", problem);
        return false;
    }
    if (!sign_record(record, signing_key)) {
        return false;
    }
    request.record = *record;

    if (!pool_add(&library->pool, &request) || !save_pool(POOL_FILE, &library->pool)) {
        return false;
    }
    printf("Request %.12s added to the pending pool: %s signed by %s with nonce %d.\n", record->record_id,
           action_name(record->action), record->signer_id, record->nonce);
    return true;
}

static bool signer_already_dropped(char dropped[][ID_SIZE], int dropped_count, const char *signer_id)
{
    for (int i = 0; i < dropped_count; i++) {
        if (strcmp(dropped[i], signer_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool request_is_still_valid(LibraryState *working, const Record *record, char problem[PROBLEM_SIZE])
{
    char expected_id[HASH_HEX_SIZE];
    compute_record_id(record, expected_id);
    if (strcmp(expected_id, record->record_id) != 0) {
        snprintf(problem, PROBLEM_SIZE, "its record_id does not match its contents");
        return false;
    }
    const Identity *signer = find_identity(&working->identities, record->signer_id);
    if (signer != NULL && !record_signature_is_valid(record, signer->public_key)) {
        snprintf(problem, PROBLEM_SIZE, "signature INVALID for %s", record->signer_id);
        return false;
    }
    return apply_record(working, record, problem);
}

/*
 * Replays the pool in submission order on top of the confirmed state and
 * drops every request that no longer holds. When a request is dropped, the
 * same signer's later requests carry nonces that can never be confirmed (a
 * nonce gap), so they fail too and are reported as needing resubmission.
 */
void prune_pool(LibrarySystem *library)
{
    LibraryState *working = clone_state(&library->state);
    if (working == NULL) {
        return;
    }

    char dropped_signers[MAX_MEMBERS + 1][ID_SIZE];
    int dropped_count = 0;
    bool changed = false;
    int index = 0;

    while (index < library->pool.count) {
        const Record *record = &library->pool.items[index].record;
        char problem[PROBLEM_SIZE];
        if (request_is_still_valid(working, record, problem)) {
            index++;
            continue;
        }

        if (signer_already_dropped(dropped_signers, dropped_count, record->signer_id)) {
            printf("Dropped request %.12s (%s by %s): it follows a dropped request from the same signer, "
                   "so nonce %d can never be confirmed. Submit it again.\n",
                   record->record_id, action_name(record->action), record->signer_id, record->nonce);
        } else {
            printf("Dropped request %.12s (%s by %s): %s\n", record->record_id, action_name(record->action),
                   record->signer_id, problem);
            if (dropped_count <= MAX_MEMBERS) {
                copy_field(dropped_signers[dropped_count++], ID_SIZE, record->signer_id);
            }
        }
        pool_remove(&library->pool, index);
        changed = true;
    }

    free_state(working);
    if (changed) {
        save_pool(POOL_FILE, &library->pool);
    }
}
