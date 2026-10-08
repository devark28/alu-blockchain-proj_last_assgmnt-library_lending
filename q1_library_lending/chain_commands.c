#include "commands.h"
#include "storage.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>

bool command_chain_view(LibrarySystem *library, int arg_count, char *args[])
{
    const BlockList *chain = &library->chain;

    if (arg_count == 0) {
        printf("Chain: %d block%s. Use chain_view <index> to see one block's records and Merkle tree.\n",
               chain->count, chain->count == 1 ? "" : "s");
        for (int i = 0; i < chain->count; i++) {
            print_block_summary(&chain->blocks[i]);
        }
        return true;
    }

    long long index;
    if (!parse_number(args[0], 0, INT_MAX, &index) || index >= chain->count) {
        printf("ERROR: block index must be a number from 0 to %d\n", chain->count - 1);
        return false;
    }
    print_block_details(&chain->blocks[index]);
    return true;
}

/*
 * Checks every block, then compares the chain with meta.dat, because a
 * chain cut short still passes the block checks. A chain that was already
 * refused at startup (for example for breaking the lending rules) stays
 * refused until chain_load succeeds.
 */
bool command_validate_chain(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    bool blocks_valid = validate_chain(&library->chain, NULL, true);

    ChainMetadata metadata;
    bool length_valid = load_metadata(&metadata) && check_chain_against_metadata(&library->chain, &metadata);
    if (blocks_valid && length_valid) {
        printf("The chain length matches %s (%d blocks).\n", META_FILE, metadata.found ? metadata.height : 0);
    }
    if (!blocks_valid || !length_valid) {
        library->chain_is_valid = false;
    }
    return library->chain_is_valid;
}

bool command_logout(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    int locked = lock_all_keys(&library->keys);
    if (locked == 0) {
        printf("No keys were unlocked.\n");
    } else {
        printf("Locked %d key%s. Private keys have been removed from memory.\n", locked, locked == 1 ? "" : "s");
    }
    return true;
}
