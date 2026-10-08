#include "library.h"

#include <stdio.h>

bool library_open(LibrarySystem *library)
{
    if (!load_registry(&library->registry, BOOKS_FILE, MEMBERS_FILE, COPIES_FILE)) {
        return false;
    }
    printf("Loaded %d books, %d copies and %d members\n", library->registry.book_count,
           library->registry.copy_count, library->registry.member_count);
    return true;
}

void library_close(LibrarySystem *library)
{
    (void)library;
}
