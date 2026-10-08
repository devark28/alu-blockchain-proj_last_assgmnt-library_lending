#include "merkle.h"

#include <stdio.h>
#include <string.h>

#define SHORT_HASH_LENGTH 12

static void hash_pair(const char *left, const char *right, char output[HASH_HEX_SIZE])
{
    char joined[HASH_HEX_SIZE * 2];
    snprintf(joined, sizeof(joined), "%s%s", left, right);
    sha256_hex(joined, output);
}

/*
 * Replaces level with the level above it and returns its size. Each parent
 * is the hash of two neighbours; a node left without a partner is paired
 * with itself, as Bitcoin does.
 */
static int build_parent_level(char level[][HASH_HEX_SIZE], int count)
{
    int parent_count = 0;
    for (int i = 0; i < count; i += 2) {
        const char *right = i + 1 < count ? level[i + 1] : level[i];
        char parent[HASH_HEX_SIZE];
        hash_pair(level[i], right, parent);
        memcpy(level[parent_count++], parent, HASH_HEX_SIZE);
    }
    return parent_count;
}

/*
 * Pairing an odd node with itself means [a, b, c] and [a, b, c, c] share a
 * root. Blocks guard against that by storing record_count in the hashed
 * header and by refusing a block that repeats a record.
 */
bool compute_merkle_root(const char leaves[][HASH_HEX_SIZE], int leaf_count, char root[HASH_HEX_SIZE])
{
    if (leaf_count < 1 || leaf_count > MAX_MERKLE_LEAVES) {
        return false;
    }

    char level[MAX_MERKLE_LEAVES][HASH_HEX_SIZE];
    memcpy(level, leaves, (size_t)leaf_count * HASH_HEX_SIZE);

    int count = leaf_count;
    while (count > 1) {
        count = build_parent_level(level, count);
    }
    memcpy(root, level[0], HASH_HEX_SIZE);
    return true;
}

static void print_level(const char *label, char level[][HASH_HEX_SIZE], int count)
{
    printf("    %-8s", label);
    for (int i = 0; i < count; i++) {
        printf(" %.*s", SHORT_HASH_LENGTH, level[i]);
    }
    printf("\n");
}

void print_merkle_tree(const char leaves[][HASH_HEX_SIZE], int leaf_count)
{
    if (leaf_count < 1 || leaf_count > MAX_MERKLE_LEAVES) {
        return;
    }

    char level[MAX_MERKLE_LEAVES][HASH_HEX_SIZE];
    memcpy(level, leaves, (size_t)leaf_count * HASH_HEX_SIZE);

    int count = leaf_count;
    print_level("leaves", level, count);
    if (count == 1) {
        printf("    root     the single leaf is the root\n");
    }
    while (count > 1) {
        count = build_parent_level(level, count);
        print_level(count == 1 ? "root" : "parents", level, count);
    }
}
