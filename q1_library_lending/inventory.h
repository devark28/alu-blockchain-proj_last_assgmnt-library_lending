#ifndef INVENTORY_H
#define INVENTORY_H

#include "registry.h"

#include <stdbool.h>

bool transition_is_allowed(CopyState from, CopyState to);
bool change_copy_state(Copy *copy, CopyState new_state);
Copy *find_available_copy(Registry *registry, const char *book_id);
int count_copies_in_state(const Registry *registry, const char *book_id, CopyState state);
int condition_drop(CopyCondition before, CopyCondition after);

void print_copy_header(void);
void print_copy_row(const Copy *copy);
void print_book_summary(const Registry *registry, const Book *book);

#endif
