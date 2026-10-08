#include "keyring.h"
#include "input.h"
#include "utils.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PASSPHRASE_SIZE 128
#define MIN_PASSPHRASE_LENGTH 8
#define MAX_UNLOCK_ATTEMPTS 3

static void key_paths(const char *id, char private_path[KEY_PATH_SIZE], char public_path[KEY_PATH_SIZE])
{
    snprintf(private_path, KEY_PATH_SIZE, "%s/%s_private.pem", KEYS_DIR, id);
    snprintf(public_path, KEY_PATH_SIZE, "%s/%s_public.pem", KEYS_DIR, id);
}

Identity *find_identity(const Keyring *keyring, const char *id)
{
    for (int i = 0; i < keyring->count; i++) {
        if (strcmp(keyring->items[i].id, id) == 0) {
            return &keyring->items[i];
        }
    }
    return NULL;
}

Identity *add_identity(Keyring *keyring, const char *id, const char *public_key_hex)
{
    EVP_PKEY *public_key = public_key_from_hex(public_key_hex);
    if (public_key == NULL) {
        return NULL;
    }

    if (keyring->count == keyring->capacity) {
        int new_capacity = keyring->capacity == 0 ? 8 : keyring->capacity * 2;
        Identity *grown = realloc(keyring->items, (size_t)new_capacity * sizeof(Identity));
        if (grown == NULL) {
            EVP_PKEY_free(public_key);
            return NULL;
        }
        keyring->items = grown;
        keyring->capacity = new_capacity;
    }

    Identity *identity = &keyring->items[keyring->count++];
    memset(identity, 0, sizeof(*identity));
    copy_field(identity->id, sizeof(identity->id), id);
    identity->public_key = public_key;
    return identity;
}

/* Asks for the passphrase up to three times and returns the decrypted key, or NULL. */
EVP_PKEY *load_identity_private_key(const char *id)
{
    char private_path[KEY_PATH_SIZE];
    char public_path[KEY_PATH_SIZE];
    key_paths(id, private_path, public_path);
    if (access(private_path, F_OK) != 0) {
        printf("ERROR: the private key for %s (%s) is missing\n", id, private_path);
        return NULL;
    }

    char label[64];
    snprintf(label, sizeof(label), "Passphrase for %s: ", id);
    for (int attempt = 1; attempt <= MAX_UNLOCK_ATTEMPTS; attempt++) {
        char passphrase[PASSPHRASE_SIZE];
        bool read_ok = prompt_passphrase(label, passphrase, sizeof(passphrase));
        EVP_PKEY *key = read_ok ? load_private_key(private_path, passphrase) : NULL;
        OPENSSL_cleanse(passphrase, sizeof(passphrase));
        if (!read_ok) {
            break;
        }
        if (key != NULL) {
            return key;
        }
        printf("Wrong passphrase (attempt %d of %d).\n", attempt, MAX_UNLOCK_ATTEMPTS);
    }
    printf("ERROR: %s could not be unlocked, action cancelled\n", id);
    return NULL;
}

/*
 * The decrypted key must match the public key registered on the chain, not
 * just the .pem file next to it, because the chain is what other parties
 * verify signatures against.
 */
bool unlock_identity(Keyring *keyring, const char *id)
{
    Identity *identity = find_identity(keyring, id);
    if (identity == NULL) {
        printf("ERROR: %s has no public key registered on the chain\n", id);
        return false;
    }
    if (identity->private_key != NULL) {
        return true;
    }

    EVP_PKEY *key = load_identity_private_key(id);
    if (key == NULL) {
        return false;
    }
    if (EVP_PKEY_eq(key, identity->public_key) != 1) {
        EVP_PKEY_free(key);
        printf("ERROR: the private key file for %s does not match the public key registered on the chain\n", id);
        return false;
    }
    identity->private_key = key;
    printf("%s unlocked.\n", id);
    return true;
}

int lock_all_identities(Keyring *keyring)
{
    int locked = 0;
    for (int i = 0; i < keyring->count; i++) {
        if (keyring->items[i].private_key != NULL) {
            EVP_PKEY_free(keyring->items[i].private_key);
            keyring->items[i].private_key = NULL;
            locked++;
        }
    }
    return locked;
}

void keyring_free(Keyring *keyring)
{
    for (int i = 0; i < keyring->count; i++) {
        EVP_PKEY_free(keyring->items[i].private_key);
        EVP_PKEY_free(keyring->items[i].public_key);
    }
    free(keyring->items);
    keyring->items = NULL;
    keyring->count = 0;
    keyring->capacity = 0;
}

bool key_files_exist(const char *id)
{
    char private_path[KEY_PATH_SIZE];
    char public_path[KEY_PATH_SIZE];
    key_paths(id, private_path, public_path);
    return access(private_path, F_OK) == 0;
}

/* The public key file kept next to the private key, used to cross-check the chain. */
EVP_PKEY *load_identity_public_key(const char *id)
{
    char private_path[KEY_PATH_SIZE];
    char public_path[KEY_PATH_SIZE];
    key_paths(id, private_path, public_path);
    if (access(public_path, F_OK) != 0) {
        return NULL;
    }
    return load_public_key(public_path);
}

static bool choose_new_passphrase(const char *id, char *passphrase, size_t size)
{
    char label[64];
    char confirmation[PASSPHRASE_SIZE];
    snprintf(label, sizeof(label), "New passphrase for %s: ", id);

    while (true) {
        if (!prompt_passphrase(label, passphrase, size)) {
            return false;
        }
        if (strlen(passphrase) < MIN_PASSPHRASE_LENGTH) {
            printf("The passphrase must be at least %d characters.\n", MIN_PASSPHRASE_LENGTH);
            continue;
        }
        if (!prompt_passphrase("Confirm passphrase: ", confirmation, sizeof(confirmation))) {
            return false;
        }
        bool matches = strcmp(passphrase, confirmation) == 0;
        OPENSSL_cleanse(confirmation, sizeof(confirmation));
        if (matches) {
            return true;
        }
        printf("The passphrases do not match, try again.\n");
    }
}

/*
 * Creates and saves a new key pair for id. The private key is returned so
 * the caller can sign with it straight away; it is the caller's to free.
 */
EVP_PKEY *create_identity_keys(const char *id)
{
    printf("Creating a %s key pair for %s.\n", SIGNATURE_CURVE, id);
    if (mkdir(KEYS_DIR, 0700) != 0 && errno != EEXIST) {
        printf("ERROR: cannot create %s/ (%s)\n", KEYS_DIR, strerror(errno));
        return NULL;
    }

    char passphrase[PASSPHRASE_SIZE];
    if (!choose_new_passphrase(id, passphrase, sizeof(passphrase))) {
        OPENSSL_cleanse(passphrase, sizeof(passphrase));
        printf("ERROR: no passphrase was set for %s\n", id);
        return NULL;
    }

    char private_path[KEY_PATH_SIZE];
    char public_path[KEY_PATH_SIZE];
    key_paths(id, private_path, public_path);

    EVP_PKEY *key = generate_key_pair();
    bool saved = key != NULL && save_key_pair(key, private_path, public_path, passphrase);
    OPENSSL_cleanse(passphrase, sizeof(passphrase));
    if (!saved) {
        EVP_PKEY_free(key);
        return NULL;
    }
    printf("Saved %s (encrypted with the passphrase) and %s.\n", private_path, public_path);
    return key;
}
