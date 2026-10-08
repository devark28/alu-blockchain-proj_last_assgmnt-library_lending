#include "inventory.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

/*
 * The only state changes a copy may make. RESERVED to BORROWED is how the
 * member at the front of a reservation queue collects the copy held for them.
 */
bool transition_is_allowed(CopyState from, CopyState to)
{
    switch (from) {
    case COPY_AVAILABLE:
        return to == COPY_BORROWED || to == COPY_LOST;
    case COPY_BORROWED:
        return to == COPY_AVAILABLE || to == COPY_RESERVED || to == COPY_LOST;
    case COPY_RESERVED:
        return to == COPY_BORROWED;
    case COPY_LOST:
        return false;
    }
    return false;
}

/*
 * Every state change goes through here. The caller sets the holder and due
 * date for BORROWED and RESERVED; leaving a loan clears them.
 */
bool change_copy_state(Copy *copy, CopyState new_state)
{
    if (!transition_is_allowed(copy->state, new_state)) {
        printf("ERROR: copy %s cannot go from %s to %s\n", copy->copy_id,
               copy_state_name(copy->state), copy_state_name(new_state));
        return false;
    }

    copy->state = new_state;
    if (new_state == COPY_AVAILABLE || new_state == COPY_LOST) {
        copy->current_holder_id[0] = '\0';
        copy->due_date = 0;
        copy->renewal_count = 0;
    }
    if (new_state == COPY_LOST) {
        copy->condition = CONDITION_LOST;
    }
    return true;
}

Copy *find_available_copy(Registry *registry, const char *book_id)
{
    for (int i = 0; i < registry->copy_count; i++) {
        Copy *copy = &registry->copies[i];
        if (strcmp(copy->book_id, book_id) == 0 && copy->state == COPY_AVAILABLE) {
            return copy;
        }
    }
    return NULL;
}

int count_copies_in_state(const Registry *registry, const char *book_id, CopyState state)
{
    int count = 0;
    for (int i = 0; i < registry->copy_count; i++) {
        const Copy *copy = &registry->copies[i];
        if (strcmp(copy->book_id, book_id) == 0 && copy->state == state) {
            count++;
        }
    }
    return count;
}

/* How many levels worse a copy came back, for example NEW to WORN is 2. */
int condition_drop(CopyCondition before, CopyCondition after)
{
    return (int)after - (int)before;
}

void print_copy_header(void)
{
    printf("  %-10s %-6s %-10s %-8s %-19s %s\n", "copy", "cond", "state", "holder", "due", "renewals");
}

void print_copy_row(const Copy *copy)
{
    char due[TIME_TEXT_SIZE];
    format_time(copy->due_date, due);
    printf("  %-10s %-6s %-10s %-8s %-19s %d\n", copy->copy_id, condition_name(copy->condition),
           copy_state_name(copy->state), copy->current_holder_id[0] != '\0' ? copy->current_holder_id : "-",
           due, copy->renewal_count);
}

void print_book_summary(const Registry *registry, const Book *book)
{
    printf("  %-8s ", book->book_id);
    print_padded(book->title, 26);
    printf(" ");
    print_padded(book->author, 26);
    printf(" %d of %d available\n", count_copies_in_state(registry, book->book_id, COPY_AVAILABLE),
           count_copies(registry, book->book_id));
}
