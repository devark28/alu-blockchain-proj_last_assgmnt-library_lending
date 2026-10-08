#ifndef KEYRING_H
#define KEYRING_H

#include "crypto.h"
#include "registry.h"

#include <stdbool.h>

#define KEYS_DIR "keys"
#define KEY_PATH_SIZE 64
#define MAX_UNLOCKED_KEYS 16

/*
 * An identity is an account whose public key was registered on the chain.
 * next_nonce is the nonce its next record must carry.
 */
typedef struct {
    char id[ID_SIZE];
    EVP_PKEY *public_key;
    int next_nonce;
} Identity;

typedef struct {
    Identity *items;
    int count;
    int capacity;
} Keyring;

/* Private keys decrypted during this session, until logout or exit. */
typedef struct {
    char id[ID_SIZE];
    EVP_PKEY *private_key;
} UnlockedKey;

typedef struct {
    UnlockedKey items[MAX_UNLOCKED_KEYS];
    int count;
} KeyStore;

Identity *find_identity(const Keyring *keyring, const char *id);
Identity *add_identity(Keyring *keyring, const char *id, const char *public_key_hex);
bool clone_keyring(const Keyring *source, Keyring *copy);
void keyring_free(Keyring *keyring);

EVP_PKEY *find_unlocked_key(const KeyStore *store, const char *id);
bool keep_unlocked_key(KeyStore *store, const char *id, EVP_PKEY *private_key);
EVP_PKEY *unlock_key(KeyStore *store, const Keyring *identities, const char *id);
int lock_all_keys(KeyStore *store);

bool key_files_exist(const char *id);
EVP_PKEY *load_identity_private_key(const char *id);
EVP_PKEY *load_identity_public_key(const char *id);
EVP_PKEY *create_identity_keys(const char *id);

#endif
