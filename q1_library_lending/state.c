#include "state.h"
#include "inventory.h"
#include "utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *REASON_CODES[] = {"DAMAGED", "WATER_DAMAGE", "LOST_BY_MEMBER", "MISSING_FROM_SHELF"};
static const int REASON_CODE_COUNT = (int)(sizeof(REASON_CODES) / sizeof(REASON_CODES[0]));

bool is_valid_reason_code(const char *code)
{
    for (int i = 0; i < REASON_CODE_COUNT; i++) {
        if (strcmp(code, REASON_CODES[i]) == 0) {
            return true;
        }
    }
    return false;
}

void print_reason_codes(void)
{
    printf("Reason codes:");
    for (int i = 0; i < REASON_CODE_COUNT; i++) {
        printf(" %s", REASON_CODES[i]);
    }
    printf("\n");
}

time_t loan_due_date(time_t borrowed_at)
{
    return borrowed_at + (time_t)LOAN_DAYS * DAY_SECONDS;
}

/* Every started day past the due date costs FINE_PER_DAY. */
int compute_fine(time_t due_date, time_t at)
{
    if (due_date == 0 || at <= due_date) {
        return 0;
    }
    long long days_late = ((long long)(at - due_date) + DAY_SECONDS - 1) / DAY_SECONDS;
    return (int)(days_late * FINE_PER_DAY);
}

LibraryState *clone_state(const LibraryState *state)
{
    LibraryState *copy = malloc(sizeof(LibraryState));
    if (copy == NULL) {
        return NULL;
    }
    copy->registry = state->registry;
    bool cloned = clone_keyring(&state->identities, &copy->identities);
    if (cloned && !clone_reservations(&state->reservations, &copy->reservations)) {
        keyring_free(&copy->identities);
        cloned = false;
    }
    if (!cloned) {
        free(copy);
        printf("ERROR: out of memory while copying the library state\n");
        return NULL;
    }
    return copy;
}

void free_state_contents(LibraryState *state)
{
    keyring_free(&state->identities);
    reservations_free(&state->reservations);
}

void free_state(LibraryState *state)
{
    if (state != NULL) {
        free_state_contents(state);
        free(state);
    }
}

/* Writes the reason a record was refused and returns false, so checks can end with return fail(...). */
static bool fail(char problem[PROBLEM_SIZE], const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(problem, PROBLEM_SIZE, format, arguments);
    va_end(arguments);
    return false;
}

/*
 * The lending fields of a record must match the registries, so a record
 * cannot name a book or member that does not exist or carry a changed title.
 */
static bool check_member(const LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    const Member *member = find_member(&state->registry, record->member_id);
    if (member == NULL) {
        return fail(problem, "member %s is not in the registry", record->member_id);
    }
    if (strcmp(member->full_name, record->member_name) != 0) {
        return fail(problem, "member name for %s does not match the registry", record->member_id);
    }
    return true;
}

static bool check_book(const LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    const Book *book = find_book(&state->registry, record->book_id);
    if (book == NULL) {
        return fail(problem, "book %s is not in the registry", record->book_id);
    }
    if (strcmp(book->title, record->book_title) != 0) {
        return fail(problem, "title for %s does not match the registry", record->book_id);
    }
    return true;
}

static Copy *check_copy(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    Copy *copy = find_copy(&state->registry, record->copy_id);
    if (copy == NULL || strcmp(copy->book_id, record->book_id) != 0) {
        fail(problem, "copy %s of %s does not exist", record->copy_id, record->book_id);
        return NULL;
    }
    return copy;
}

static Copy *check_loan(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    Copy *copy = check_copy(state, record, problem);
    if (copy == NULL) {
        return NULL;
    }
    if (copy->state != COPY_BORROWED || strcmp(copy->current_holder_id, record->member_id) != 0) {
        fail(problem, "copy %s is not on loan to %s", copy->copy_id, record->member_id);
        return NULL;
    }
    return copy;
}

static bool member_holds_title(const LibraryState *state, const char *member_id, const char *book_id)
{
    for (int i = 0; i < state->registry.copy_count; i++) {
        const Copy *copy = &state->registry.copies[i];
        if (strcmp(copy->book_id, book_id) == 0 && strcmp(copy->current_holder_id, member_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool apply_borrow(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    if (!check_member(state, record, problem) || !check_book(state, record, problem)) {
        return false;
    }
    Copy *copy = check_copy(state, record, problem);
    if (copy == NULL) {
        return false;
    }

    Member *member = find_member(&state->registry, record->member_id);
    bool held_for_member = copy->state == COPY_RESERVED && strcmp(copy->current_holder_id, member->member_id) == 0;
    if (member->fines_owed > 0) {
        return fail(problem, "%s has %d coins of unpaid fines", member->member_id, member->fines_owed);
    }
    if (copy->state != COPY_AVAILABLE && !held_for_member) {
        return fail(problem, "copy %s is not available (%s)", copy->copy_id, copy_state_name(copy->state));
    }
    if (record->due_date != loan_due_date(record->timestamp)) {
        return fail(problem, "due date of %s does not match the loan period", copy->copy_id);
    }

    change_copy_state(copy, COPY_BORROWED);
    copy_field(copy->current_holder_id, ID_SIZE, member->member_id);
    copy->due_date = record->due_date;
    remove_reservation(&state->reservations, record->book_id, member->member_id);
    return true;
}

/*
 * A returned copy goes to the member at the head of its title's reservation
 * queue if there is one, and only becomes generally available otherwise.
 */
static bool apply_return(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    if (!check_member(state, record, problem) || !check_book(state, record, problem)) {
        return false;
    }
    Copy *copy = check_loan(state, record, problem);
    if (copy == NULL) {
        return false;
    }

    CopyCondition condition;
    if (!parse_condition(record->condition, &condition) || condition == CONDITION_LOST) {
        return fail(problem, "returned condition '%s' is not NEW, GOOD or WORN", record->condition);
    }
    if (condition_drop(copy->condition, condition) < 0) {
        return fail(problem, "condition cannot improve from %s to %s", condition_name(copy->condition),
                    record->condition);
    }
    if (record->reason_code[0] != '\0' && !is_valid_reason_code(record->reason_code)) {
        return fail(problem, "unknown reason code %s", record->reason_code);
    }
    int fine = compute_fine(copy->due_date, record->timestamp);
    if (record->fine_amount != fine) {
        return fail(problem, "fine %d does not match the %d owed for this return", record->fine_amount,
                 fine);
    }

    Member *member = find_member(&state->registry, record->member_id);
    member->fines_owed += fine;
    copy->condition = condition;

    const ReservationQueue *queue = find_queue(&state->reservations, record->book_id);
    if (queue != NULL && queue->head != NULL) {
        time_t reservation_date = queue->head->reservation_date;
        char next_member[ID_SIZE];
        dequeue_reservation(&state->reservations, record->book_id, next_member);
        change_copy_state(copy, COPY_RESERVED);
        copy_field(copy->current_holder_id, ID_SIZE, next_member);
        copy->reserved_at = reservation_date;
    } else {
        change_copy_state(copy, COPY_AVAILABLE);
    }
    return true;
}

static bool apply_reserve(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    if (!check_member(state, record, problem) || !check_book(state, record, problem)) {
        return false;
    }
    if (count_copies_in_state(&state->registry, record->book_id, COPY_AVAILABLE) > 0) {
        return fail(problem, "%s has a copy available, borrow it instead", record->book_id);
    }
    if (find_reservation(&state->reservations, record->book_id, record->member_id) != NULL) {
        return fail(problem, "%s is already in the queue for %s", record->member_id, record->book_id);
    }
    if (member_holds_title(state, record->member_id, record->book_id)) {
        return fail(problem, "%s already holds a copy of %s", record->member_id, record->book_id);
    }
    if (!enqueue_reservation(&state->reservations, record->book_id, record->member_id, record->timestamp)) {
        return fail(problem, "out of memory while reserving %s", record->book_id);
    }
    return true;
}

static bool apply_renew(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    if (!check_member(state, record, problem) || !check_book(state, record, problem)) {
        return false;
    }
    Copy *copy = check_loan(state, record, problem);
    if (copy == NULL) {
        return false;
    }

    const ReservationQueue *queue = find_queue(&state->reservations, record->book_id);
    if (queue != NULL && queue->length > 0) {
        return fail(problem, "another member is waiting for %s (%s)", record->book_id, queue->head->member_id);
    }
    if (copy->renewal_count >= MAX_RENEWALS) {
        return fail(problem, "copy %s has already been renewed the maximum number of times", copy->copy_id);
    }
    if (record->timestamp > copy->due_date) {
        return fail(problem, "copy %s is overdue and must be returned, not renewed", copy->copy_id);
    }
    if (record->due_date != copy->due_date + (time_t)RENEWAL_DAYS * DAY_SECONDS) {
        return fail(problem, "new due date of %s does not match the renewal period", copy->copy_id);
    }

    copy->due_date = record->due_date;
    copy->renewal_count++;
    return true;
}

static bool apply_overdue(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    if (!check_member(state, record, problem) || !check_book(state, record, problem)) {
        return false;
    }
    Copy *copy = check_loan(state, record, problem);
    if (copy == NULL) {
        return false;
    }
    if (record->timestamp <= copy->due_date) {
        return fail(problem, "copy %s is not past its due date", copy->copy_id);
    }
    if (copy->overdue_recorded) {
        return fail(problem, "copy %s is already recorded as overdue", copy->copy_id);
    }
    if (record->fine_amount != compute_fine(copy->due_date, record->timestamp)) {
        return fail(problem, "fine on the overdue record for %s is wrong", copy->copy_id);
    }
    copy->overdue_recorded = true;
    return true;
}

static bool apply_lost(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    if (!check_book(state, record, problem)) {
        return false;
    }
    Copy *copy = check_copy(state, record, problem);
    if (copy == NULL) {
        return false;
    }
    if (!is_valid_reason_code(record->reason_code)) {
        return fail(problem, "marking %s lost needs a valid reason code", copy->copy_id);
    }
    if (!transition_is_allowed(copy->state, COPY_LOST)) {
        return fail(problem, "copy %s cannot be marked lost while %s", copy->copy_id, copy_state_name(copy->state));
    }
    change_copy_state(copy, COPY_LOST);
    return true;
}

static bool apply_registration(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    bool is_librarian = strcmp(record->member_id, LIBRARIAN_ID) == 0;
    if (!is_librarian && !check_member(state, record, problem)) {
        return false;
    }
    if (find_identity(&state->identities, record->member_id) != NULL) {
        return fail(problem, "%s is already registered", record->member_id);
    }
    if (add_identity(&state->identities, record->member_id, record->public_key_hex) == NULL) {
        return fail(problem, "public key for %s is not a valid key", record->member_id);
    }
    return true;
}

static bool apply_action(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    switch (record->action) {
    case ACTION_BORROWED:
        return apply_borrow(state, record, problem);
    case ACTION_RETURNED:
        return apply_return(state, record, problem);
    case ACTION_RESERVED:
        return apply_reserve(state, record, problem);
    case ACTION_RENEWED:
        return apply_renew(state, record, problem);
    case ACTION_OVERDUE:
        return apply_overdue(state, record, problem);
    case ACTION_LOST:
        return apply_lost(state, record, problem);
    case ACTION_REGISTERED:
        return apply_registration(state, record, problem);
    case ACTION_TRANSFER:
    case ACTION_FINE_PAID:
        return fail(problem, "%s records are not supported yet", action_name(record->action));
    }
    return false;
}

/*
 * The one place the lending rules live. It checks that the record's signer
 * is registered and used their next nonce, then checks and applies the
 * action. Nothing changes unless every check passes. Signatures are verified
 * by the caller, because replaying an already validated chain does not need
 * to verify them twice.
 */
bool apply_record(LibraryState *state, const Record *record, char problem[PROBLEM_SIZE])
{
    bool is_bootstrap = record->action == ACTION_REGISTERED && state->identities.count == 0;
    problem[0] = '\0';

    if (!record_signer_is_authorized(record)) {
        return fail(problem, "%s may not sign a %s record", record->signer_id, action_name(record->action));
    }
    if (is_bootstrap) {
        if (strcmp(record->member_id, LIBRARIAN_ID) != 0 || record->nonce != 0) {
            return fail(problem, "the first record must be the librarian registering their key");
        }
    } else {
        const Identity *signer = find_identity(&state->identities, record->signer_id);
        if (signer == NULL) {
            return fail(problem, "%s has no registered public key", record->signer_id);
        }
        if (record->nonce != signer->next_nonce) {
            return fail(problem, "nonce %d from %s, expected %d", record->nonce, record->signer_id,
                     signer->next_nonce);
        }
    }

    if (!apply_action(state, record, problem)) {
        return false;
    }
    find_identity(&state->identities, record->signer_id)->next_nonce++;
    return true;
}

/* Rebuilds the state by applying every confirmed record in chain order. */
bool replay_chain(LibraryState *state, const BlockList *chain)
{
    for (int b = 0; b < chain->count; b++) {
        const Block *block = &chain->blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            char problem[PROBLEM_SIZE];
            if (!apply_record(state, &block->records[r], problem)) {
                printf("ERROR: block %d record %d breaks the lending rules: %s\n", b, r, problem);
                return false;
            }
        }
    }
    return true;
}
