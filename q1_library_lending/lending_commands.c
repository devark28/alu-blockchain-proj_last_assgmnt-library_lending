#include "commands.h"
#include "inventory.h"
#include "mining.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void start_record(Record *record, RecordAction action, const char *signer_id)
{
    memset(record, 0, sizeof(*record));
    record->action = action;
    record->timestamp = time(NULL);
    copy_field(record->signer_id, ID_SIZE, signer_id);
}

/* Title and name are copied from the registries at the time of the request. */
static void fill_lending_fields(Record *record, const Book *book, const char *copy_id, const Member *member)
{
    copy_field(record->book_id, ID_SIZE, book->book_id);
    copy_field(record->book_title, TITLE_SIZE, book->title);
    copy_field(record->copy_id, ID_SIZE, copy_id);
    if (member != NULL) {
        copy_field(record->member_id, ID_SIZE, member->member_id);
        copy_field(record->member_name, NAME_SIZE, member->full_name);
    }
}

/* A copy held for this member comes first, then any available copy. */
static const Copy *choose_copy_to_borrow(const Registry *registry, const char *book_id, const char *member_id)
{
    const Copy *available = NULL;
    for (int i = 0; i < registry->copy_count; i++) {
        const Copy *copy = &registry->copies[i];
        if (strcmp(copy->book_id, book_id) != 0) {
            continue;
        }
        if (copy->state == COPY_RESERVED && strcmp(copy->current_holder_id, member_id) == 0) {
            return copy;
        }
        if (copy->state == COPY_AVAILABLE && available == NULL) {
            available = copy;
        }
    }
    return available;
}

static void print_due_date(const char *label, time_t due_date)
{
    char text[TIME_TEXT_SIZE];
    format_time(due_date, text);
    printf("%s %s\n", label, text);
}

bool command_register_member(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    Registry *registry = &library->state.registry;
    Member member = {0};

    if (!is_valid_member_id(args[0])) {
        printf("ERROR: %s is not a valid member ID (expected ALU followed by 3 to 6 digits)\n", args[0]);
        return false;
    }
    if (find_member(registry, args[0]) != NULL) {
        printf("ERROR: member %s is already registered\n", args[0]);
        return false;
    }
    if (!is_valid_text_field(args[1]) || !is_valid_text_field(args[2])
        || !copy_field(member.member_id, ID_SIZE, args[0])
        || !copy_field(member.full_name, NAME_SIZE, args[1])
        || !copy_field(member.course_code, COURSE_SIZE, args[2])) {
        printf("ERROR: the name (up to %d characters) and course code (up to %d) must not be empty or contain "
               "commas, '|' or quotes\n", NAME_SIZE - 1, COURSE_SIZE - 1);
        return false;
    }
    if (unlock_librarian(library) == NULL || !add_member(registry, MEMBERS_FILE, &member)) {
        return false;
    }
    printf("Added %s (%s, %s) to %s.\n", member.member_id, member.full_name, member.course_code, MEMBERS_FILE);
    return unlock_member(library, member.member_id) != NULL;
}

bool command_member_nonce(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    if (require_member(&library->state.registry, args[0]) == NULL) {
        return false;
    }

    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return false;
    }
    const Identity *confirmed = find_identity(&library->state.identities, args[0]);
    const Identity *pending = find_identity(&effective->identities, args[0]);
    if (pending == NULL) {
        printf("%s has no key on the chain yet; their first request will use nonce 0.\n", args[0]);
    } else {
        printf("%s: next confirmed nonce %d, next new request uses nonce %d (%d pending)\n", args[0],
               confirmed != NULL ? confirmed->next_nonce : 0, pending->next_nonce,
               pending->next_nonce - (confirmed != NULL ? confirmed->next_nonce : 0));
    }
    free_state(effective);
    return true;
}

bool command_borrow_book(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const Book *book = find_book(&library->state.registry, args[0]);
    const Member *member = find_member(&library->state.registry, args[1]);
    if (book == NULL || member == NULL) {
        printf("ERROR: Book or Member not found\n");
        return false;
    }
    if (!require_writable_chain(library)) {
        return false;
    }

    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return false;
    }
    int fines_owed = find_member(&effective->registry, member->member_id)->fines_owed;
    const Copy *copy = choose_copy_to_borrow(&effective->registry, book->book_id, member->member_id);
    char copy_id[ID_SIZE] = "";
    if (copy != NULL) {
        copy_field(copy_id, ID_SIZE, copy->copy_id);
    }
    free_state(effective);

    if (fines_owed > 0) {
        printf("ERROR: %s has %d coins of unpaid fines; settle them before borrowing\n", member->member_id,
               fines_owed);
        return false;
    }
    if (copy_id[0] == '\0') {
        printf("ERROR: no copy of %s is available. Join the queue with reserve_book %s %s\n", book->book_id,
               book->book_id, member->member_id);
        return false;
    }

    EVP_PKEY *member_key = unlock_member(library, member->member_id);
    if (member_key == NULL) {
        return false;
    }
    Record record;
    start_record(&record, ACTION_BORROWED, member->member_id);
    fill_lending_fields(&record, book, copy_id, member);
    record.due_date = loan_due_date(record.timestamp);
    if (!submit_request(library, &record, member_key)) {
        return false;
    }
    printf("%s borrows copy %s of %s once the block is mined.\n", member->member_id, copy_id, book->title);
    print_due_date("Due:", record.due_date);
    return true;
}

/*
 * A return closes the loan recorded on the chain, so the copy must have a
 * confirmed BORROWED record. The fine is worked out here and checked again
 * by the lending rules when the record is mined.
 */
bool command_return_book(LibrarySystem *library, int arg_count, char *args[])
{
    const Registry *registry = &library->state.registry;
    const Copy *copy = require_copy(registry, args[0]);
    if (copy == NULL) {
        return false;
    }

    int block_index;
    const Record *borrow = find_latest_copy_record(&library->chain, copy->copy_id, ACTION_BORROWED, &block_index);
    if (copy->state != COPY_BORROWED || borrow == NULL) {
        printf("ERROR: copy %s has no open loan on the chain\n", copy->copy_id);
        return false;
    }

    CopyCondition condition;
    if (!parse_condition(args[1], &condition) || condition == CONDITION_LOST) {
        printf("ERROR: condition must be NEW, GOOD or WORN (use mark_lost for a lost copy)\n");
        return false;
    }
    const char *reason_code = arg_count == 3 ? args[2] : "";
    if (reason_code[0] != '\0' && !is_valid_reason_code(reason_code)) {
        printf("ERROR: unknown reason code %s. ", reason_code);
        print_reason_codes();
        return false;
    }

    EVP_PKEY *librarian_key = unlock_librarian(library);
    if (librarian_key == NULL) {
        return false;
    }
    Record record;
    start_record(&record, ACTION_RETURNED, LIBRARIAN_ID);
    fill_lending_fields(&record, find_book(registry, copy->book_id), copy->copy_id,
                        find_member(registry, copy->current_holder_id));
    record.due_date = copy->due_date;
    record.fine_amount = compute_fine(copy->due_date, record.timestamp);
    copy_field(record.condition, CONDITION_TEXT_SIZE, condition_name(condition));
    copy_field(record.reason_code, REASON_SIZE, reason_code);

    if (!submit_request(library, &record, librarian_key)) {
        return false;
    }
    printf("This closes the loan recorded in block %d.\n", block_index);
    if (record.fine_amount > 0) {
        printf("Returned late: fine of %d coins (%d per started day).\n", record.fine_amount, FINE_PER_DAY);
    } else {
        printf("Returned on time.\n");
    }

    const ReservationQueue *queue = find_queue(&library->state.reservations, copy->book_id);
    if (queue != NULL && queue->head != NULL) {
        printf("Once confirmed, the copy is held for %s, first in the reservation queue.\n", queue->head->member_id);
    }
    return true;
}

bool command_renew_book(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    if (require_copy(&library->state.registry, args[0]) == NULL || !require_writable_chain(library)) {
        return false;
    }

    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return false;
    }
    Copy copy = *find_copy(&effective->registry, args[0]);
    free_state(effective);
    if (copy.state != COPY_BORROWED) {
        printf("ERROR: copy %s is not on loan\n", copy.copy_id);
        return false;
    }

    EVP_PKEY *member_key = unlock_member(library, copy.current_holder_id);
    if (member_key == NULL) {
        return false;
    }
    Record record;
    start_record(&record, ACTION_RENEWED, copy.current_holder_id);
    fill_lending_fields(&record, find_book(&library->state.registry, copy.book_id), copy.copy_id,
                        find_member(&library->state.registry, copy.current_holder_id));
    record.due_date = copy.due_date + (time_t)RENEWAL_DAYS * DAY_SECONDS;
    if (!submit_request(library, &record, member_key)) {
        return false;
    }
    print_due_date("New due date once confirmed:", record.due_date);
    return true;
}

bool command_reserve_book(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const Book *book = find_book(&library->state.registry, args[0]);
    const Member *member = find_member(&library->state.registry, args[1]);
    if (book == NULL || member == NULL) {
        printf("ERROR: Book or Member not found\n");
        return false;
    }

    EVP_PKEY *member_key = unlock_member(library, member->member_id);
    if (member_key == NULL) {
        return false;
    }
    Record record;
    start_record(&record, ACTION_RESERVED, member->member_id);
    fill_lending_fields(&record, book, "", member);
    if (!submit_request(library, &record, member_key)) {
        return false;
    }
    const ReservationQueue *queue = find_queue(&library->state.reservations, book->book_id);
    printf("%s joins the queue for %s at position %d once the block is mined.\n", member->member_id, book->title,
           (queue != NULL ? queue->length : 0) + 1);
    return true;
}

static void print_queue(const Registry *registry, const ReservationQueue *queue)
{
    const Book *book = find_book(registry, queue->book_id);
    printf("  %s %s: %d waiting\n", queue->book_id, book != NULL ? book->title : "", queue->length);
    int position = 1;
    for (const ReservationNode *node = queue->head; node != NULL; node = node->next) {
        char reserved[TIME_TEXT_SIZE];
        format_time(node->reservation_date, reserved);
        printf("    %d. %s since %s\n", position++, node->member_id, reserved);
    }
}

static void print_held_copies(const Registry *registry, const char *book_id)
{
    for (int i = 0; i < registry->copy_count; i++) {
        const Copy *copy = &registry->copies[i];
        if (copy->state == COPY_RESERVED && (book_id == NULL || strcmp(copy->book_id, book_id) == 0)) {
            printf("  copy %s is held for %s to collect with borrow_book %s %s\n", copy->copy_id,
                   copy->current_holder_id, copy->book_id, copy->current_holder_id);
        }
    }
}

bool command_reservation_status(LibrarySystem *library, int arg_count, char *args[])
{
    const Registry *registry = &library->state.registry;
    const ReservationList *reservations = &library->state.reservations;

    if (arg_count == 1) {
        if (require_book(registry, args[0]) == NULL) {
            return false;
        }
        const ReservationQueue *queue = find_queue(reservations, args[0]);
        if (queue == NULL || queue->length == 0) {
            printf("No one is waiting for %s.\n", args[0]);
        } else {
            print_queue(registry, queue);
        }
        print_held_copies(registry, args[0]);
        return true;
    }

    int shown = 0;
    printf("Reservation queues\n");
    for (int i = 0; i < reservations->count; i++) {
        if (reservations->queues[i].length > 0) {
            print_queue(registry, &reservations->queues[i]);
            shown++;
        }
    }
    if (shown == 0) {
        printf("  no one is waiting for any book\n");
    }
    print_held_copies(registry, NULL);
    return true;
}

bool command_mark_lost(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    const Registry *registry = &library->state.registry;
    const Copy *copy = require_copy(registry, args[0]);
    if (copy == NULL) {
        return false;
    }
    if (!is_valid_reason_code(args[1])) {
        printf("ERROR: marking a copy lost needs a reason code. ");
        print_reason_codes();
        return false;
    }

    EVP_PKEY *librarian_key = unlock_librarian(library);
    if (librarian_key == NULL) {
        return false;
    }
    Record record;
    start_record(&record, ACTION_LOST, LIBRARIAN_ID);
    const Member *holder = copy->state == COPY_BORROWED ? find_member(registry, copy->current_holder_id) : NULL;
    fill_lending_fields(&record, find_book(registry, copy->book_id), copy->copy_id, holder);
    copy_field(record.reason_code, REASON_SIZE, args[1]);
    return submit_request(library, &record, librarian_key);
}

/* Records one OVERDUE request for every loan past its due date that has none yet. */
static int flag_overdue_loans(LibrarySystem *library, EVP_PKEY *librarian_key)
{
    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return 0;
    }

    int flagged = 0;
    time_t now = time(NULL);
    for (int i = 0; i < effective->registry.copy_count; i++) {
        const Copy *copy = &effective->registry.copies[i];
        if (copy->state != COPY_BORROWED || copy->overdue_recorded || now <= copy->due_date) {
            continue;
        }
        Record record;
        start_record(&record, ACTION_OVERDUE, LIBRARIAN_ID);
        fill_lending_fields(&record, find_book(&effective->registry, copy->book_id), copy->copy_id,
                            find_member(&effective->registry, copy->current_holder_id));
        record.due_date = copy->due_date;
        record.fine_amount = compute_fine(copy->due_date, record.timestamp);
        printf("Overdue: %s held by %s, fine so far %d.\n", copy->copy_id, copy->current_holder_id,
               record.fine_amount);
        if (submit_request(library, &record, librarian_key)) {
            flagged++;
        }
    }
    free_state(effective);
    return flagged;
}

static int count_unflagged_overdue(const LibrarySystem *library)
{
    LibraryState *effective = build_effective_state(library);
    if (effective == NULL) {
        return 0;
    }
    int count = 0;
    time_t now = time(NULL);
    for (int i = 0; i < effective->registry.copy_count; i++) {
        const Copy *copy = &effective->registry.copies[i];
        if (copy->state == COPY_BORROWED && !copy->overdue_recorded && now > copy->due_date) {
            count++;
        }
    }
    free_state(effective);
    return count;
}

/*
 * Runs before every command. Loans that have passed their due date are
 * recorded as OVERDUE straight away when the librarian's key is unlocked;
 * otherwise a reminder is printed, because signing needs the passphrase.
 */
void scan_for_overdue_loans(LibrarySystem *library)
{
    if (!library->chain_is_valid) {
        return;
    }
    int waiting = count_unflagged_overdue(library);
    if (waiting == 0) {
        return;
    }
    EVP_PKEY *librarian_key = find_unlocked_key(&library->keys, LIBRARIAN_ID);
    if (librarian_key != NULL) {
        flag_overdue_loans(library, librarian_key);
    } else {
        printf("Note: %d loan%s past due and not yet recorded as overdue. Run check_overdue to record %s.\n",
               waiting, waiting == 1 ? " is" : "s are", waiting == 1 ? "it" : "them");
    }
}

static void print_overdue_loans(const Registry *registry)
{
    time_t now = time(NULL);
    int shown = 0;
    printf("Loans past their due date\n");
    for (int i = 0; i < registry->copy_count; i++) {
        const Copy *copy = &registry->copies[i];
        if (copy->state != COPY_BORROWED || now <= copy->due_date) {
            continue;
        }
        char due[TIME_TEXT_SIZE];
        format_time(copy->due_date, due);
        printf("  %-10s %-8s due %s  fine so far %d%s\n", copy->copy_id, copy->current_holder_id, due,
               compute_fine(copy->due_date, now), copy->overdue_recorded ? "  (recorded as OVERDUE)" : "");
        shown++;
    }
    if (shown == 0) {
        printf("  none\n");
    }
}

bool command_check_overdue(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    if (count_unflagged_overdue(library) > 0) {
        EVP_PKEY *librarian_key = unlock_librarian(library);
        if (librarian_key == NULL) {
            return false;
        }
        int flagged = flag_overdue_loans(library, librarian_key);
        printf("%d OVERDUE request%s added to the pending pool.\n", flagged, flagged == 1 ? "" : "s");
    }
    print_overdue_loans(&library->state.registry);
    return true;
}

static void print_member_fines(const Registry *registry, const Member *member, time_t now)
{
    int accruing = 0;
    for (int i = 0; i < registry->copy_count; i++) {
        const Copy *copy = &registry->copies[i];
        if (copy->state == COPY_BORROWED && strcmp(copy->current_holder_id, member->member_id) == 0) {
            accruing += compute_fine(copy->due_date, now);
        }
    }
    printf("  %-8s owed %-4d still accruing on overdue loans %d\n", member->member_id, member->fines_owed, accruing);
}

/*
 * Fines are owed once a late return is confirmed. Copies still out past
 * their due date show the fine accruing so far, FINE_PER_DAY per started day.
 */
bool command_view_fines(LibrarySystem *library, int arg_count, char *args[])
{
    const Registry *registry = &library->state.registry;
    time_t now = time(NULL);
    printf("Fines (%d coins per started day late)\n", FINE_PER_DAY);

    if (arg_count == 1) {
        const Member *member = require_member(registry, args[0]);
        if (member == NULL) {
            return false;
        }
        print_member_fines(registry, member, now);
        return true;
    }
    for (int i = 0; i < registry->member_count; i++) {
        print_member_fines(registry, &registry->members[i], now);
    }
    return true;
}

bool command_pool_view(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    (void)args;
    print_pool(&library->pool);
    return true;
}

bool command_mine_solo(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    if (require_member(&library->state.registry, args[0]) == NULL || !require_writable_chain(library)) {
        return false;
    }

    int mined = 0;
    Block block;
    LibraryState *working;
    while (assemble_block(library, &block, &working) > 0) {
        prepare_block(&block, &library->chain, library->difficulty, args[0]);
        unsigned long attempts = 0;
        search_nonce(&block, ULONG_MAX, &attempts);
        if (!commit_block(library, &block, working)) {
            return false;
        }
        printf("Block %d mined by %s: %d record%s, difficulty %d, nonce %lu, %lu hash attempts\n  hash %s\n",
               block.index, args[0], block.record_count, block.record_count == 1 ? "" : "s", block.difficulty,
               block.nonce, attempts, block.hash);
        mined++;
    }

    if (mined == 0) {
        printf("Nothing to mine: no pending request can be confirmed right now.\n");
    }
    return mined > 0;
}
