#ifndef FRAUD_H
#define FRAUD_H

#include "library.h"

#include <stdbool.h>

#define MAX_CONCURRENT_BORROWS 3
#define REBORROW_WINDOW_SECONDS DAY_SECONDS

bool request_id_exists(const LibrarySystem *library, const char *record_id);
bool detect_misuse(const LibrarySystem *library, const LibraryState *effective, const Record *record,
                   char reason[FLAG_REASON_SIZE]);

#endif
