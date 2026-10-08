#ifndef KEYRING_H
#define KEYRING_H

#include "crypto.h"
#include "registry.h"

#include <stdbool.h>

#define KEYS_DIR "keys"
#define KEY_PATH_SIZE 64

/*
 * An identity is an account whose public key was registered on the chain.
 * next_nonce is the nonce its next confirmed record must carry. The private
 * key is only in memory after the owner unlocks it with their passphrase.
 */
typedef struct {
    char id[ID_SIZE];
    EVP_PKEY *public_key;
    EVP_PKEY *private_key;
    int next_nonce;
} Identity;

typedef struct {
    Identity *items;
    int count;
    int capacity;
} Keyring;

Identity *find_identity(const Keyring *keyring, const char *id);
Identity *add_identity(Keyring *keyring, const char *id, const char *public_key_hex);
bool unlock_identity(Keyring *keyring, const char *id);
int lock_all_identities(Keyring *keyring);
void keyring_free(Keyring *keyring);

bool key_files_exist(const char *id);
EVP_PKEY *load_identity_private_key(const char *id);
EVP_PKEY *load_identity_public_key(const char *id);
EVP_PKEY *create_identity_keys(const char *id);

#endif
