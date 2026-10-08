#ifndef CRYPTO_H
#define CRYPTO_H

#include <openssl/evp.h>
#include <stdbool.h>
#include <stddef.h>

#define HASH_HEX_SIZE 65
#define MAX_SIGNATURE_SIZE 72
#define SIGNATURE_HEX_SIZE (MAX_SIGNATURE_SIZE * 2 + 1)
#define MAX_PUBLIC_KEY_SIZE 100
#define PUBLIC_KEY_HEX_SIZE (MAX_PUBLIC_KEY_SIZE * 2 + 1)
#define SIGNATURE_CURVE "secp256k1"

void sha256_hex(const char *data, char output[HASH_HEX_SIZE]);

EVP_PKEY *generate_key_pair(void);
bool save_key_pair(EVP_PKEY *key, const char *private_path, const char *public_path, const char *passphrase);
EVP_PKEY *load_private_key(const char *path, const char *passphrase);
EVP_PKEY *load_public_key(const char *path);
bool public_key_to_hex(EVP_PKEY *key, char hex[PUBLIC_KEY_HEX_SIZE]);
EVP_PKEY *public_key_from_hex(const char *hex);

bool sign_data(EVP_PKEY *private_key, const char *data, unsigned char signature[MAX_SIGNATURE_SIZE]);
bool verify_signature(EVP_PKEY *public_key, const char *data, const unsigned char signature[MAX_SIGNATURE_SIZE]);
size_t signature_length(const unsigned char signature[MAX_SIGNATURE_SIZE]);

void bytes_to_hex(const unsigned char *bytes, size_t length, char *hex);
int hex_to_bytes(const char *hex, unsigned char *bytes, size_t max_length);

#endif
