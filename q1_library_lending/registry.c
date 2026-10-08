#include "registry.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE 256
#define FIELDS_PER_LINE 3
#define BOOK_PREFIX "BK"
#define MEMBER_PREFIX "ALU"
#define MIN_ID_DIGITS 3
#define MAX_ID_DIGITS 6
#define MAX_COPY_DIGITS 3
#define MAX_COPY_NUMBER 999

typedef bool (*LineHandler)(Registry *registry, char *fields[FIELDS_PER_LINE], const char *path, int line_number);

static const char *CONDITION_NAMES[] = {"NEW", "GOOD", "WORN", "LOST"};
static const char *STATE_NAMES[] = {"AVAILABLE", "BORROWED", "RESERVED", "LOST"};

static bool has_digits_only(const char *text, int min_digits, int max_digits)
{
    int length = (int)strlen(text);
    if (length < min_digits || length > max_digits) {
        return false;
    }
    for (int i = 0; i < length; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

bool is_valid_book_id(const char *id)
{
    size_t prefix_length = strlen(BOOK_PREFIX);
    return strncmp(id, BOOK_PREFIX, prefix_length) == 0
           && has_digits_only(id + prefix_length, MIN_ID_DIGITS, MAX_ID_DIGITS);
}

bool is_valid_member_id(const char *id)
{
    size_t prefix_length = strlen(MEMBER_PREFIX);
    return strncmp(id, MEMBER_PREFIX, prefix_length) == 0
           && has_digits_only(id + prefix_length, MIN_ID_DIGITS, MAX_ID_DIGITS);
}

/* A copy ID is its book's ID, a dash and a copy number, for example BK001-2. */
bool is_valid_copy_id(const char *id)
{
    const char *dash = strrchr(id, '-');
    if (dash == NULL || dash - id >= ID_SIZE) {
        return false;
    }

    char book_id[ID_SIZE];
    memcpy(book_id, id, (size_t)(dash - id));
    book_id[dash - id] = '\0';
    return is_valid_book_id(book_id) && has_digits_only(dash + 1, 1, MAX_COPY_DIGITS);
}

/* Commas and '|' separate fields in the data files, so neither may appear. */
bool is_valid_text_field(const char *text)
{
    if (text[0] == '\0') {
        return false;
    }
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ',' || *p == '|' || *p == '"' || iscntrl((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

const char *condition_name(CopyCondition condition)
{
    return CONDITION_NAMES[condition];
}

bool parse_condition(const char *text, CopyCondition *condition)
{
    for (int i = CONDITION_NEW; i <= CONDITION_LOST; i++) {
        if (strcmp(text, CONDITION_NAMES[i]) == 0) {
            *condition = (CopyCondition)i;
            return true;
        }
    }
    return false;
}

const char *copy_state_name(CopyState state)
{
    return STATE_NAMES[state];
}

static bool split_record(char *line, char *fields[FIELDS_PER_LINE])
{
    if (split_line(line, ',', fields, FIELDS_PER_LINE) != FIELDS_PER_LINE) {
        return false;
    }
    for (int i = 0; i < FIELDS_PER_LINE; i++) {
        trim_whitespace(fields[i]);
        if (!is_valid_text_field(fields[i])) {
            return false;
        }
    }
    return true;
}

static bool add_book_line(Registry *registry, char *fields[FIELDS_PER_LINE], const char *path, int line_number)
{
    Book book;
    if (!is_valid_book_id(fields[0])
        || !copy_field(book.book_id, sizeof(book.book_id), fields[0])
        || !copy_field(book.title, sizeof(book.title), fields[1])
        || !copy_field(book.author, sizeof(book.author), fields[2])) {
        fprintf(stderr, "WARNING: %s line %d has an invalid book ID or a field that is too long, skipped\n",
                path, line_number);
        return false;
    }
    if (find_book(registry, book.book_id) != NULL) {
        fprintf(stderr, "WARNING: %s line %d repeats book ID %s, skipped\n", path, line_number, book.book_id);
        return false;
    }
    registry->books[registry->book_count++] = book;
    return true;
}

static bool add_member_line(Registry *registry, char *fields[FIELDS_PER_LINE], const char *path, int line_number)
{
    Member member;
    if (!is_valid_member_id(fields[0])
        || !copy_field(member.member_id, sizeof(member.member_id), fields[0])
        || !copy_field(member.full_name, sizeof(member.full_name), fields[1])
        || !copy_field(member.course_code, sizeof(member.course_code), fields[2])) {
        fprintf(stderr, "WARNING: %s line %d has an invalid member ID or a field that is too long, skipped\n",
                path, line_number);
        return false;
    }
    if (find_member(registry, member.member_id) != NULL) {
        fprintf(stderr, "WARNING: %s line %d repeats member ID %s, skipped\n", path, line_number, member.member_id);
        return false;
    }
    registry->members[registry->member_count++] = member;
    return true;
}

/* Copies must belong to a loaded book, and the copy ID must start with that book's ID. */
static bool add_copy_line(Registry *registry, char *fields[FIELDS_PER_LINE], const char *path, int line_number)
{
    Copy copy = {0};
    size_t book_id_length = strlen(fields[1]);
    bool id_matches_book = strncmp(fields[0], fields[1], book_id_length) == 0 && fields[0][book_id_length] == '-';

    if (!is_valid_copy_id(fields[0]) || !id_matches_book
        || !parse_condition(fields[2], &copy.condition) || copy.condition == CONDITION_LOST) {
        fprintf(stderr, "WARNING: %s line %d has an invalid copy ID or condition, skipped\n", path, line_number);
        return false;
    }
    if (find_book(registry, fields[1]) == NULL) {
        fprintf(stderr, "WARNING: %s line %d refers to unknown book %s, skipped\n", path, line_number, fields[1]);
        return false;
    }
    if (find_copy(registry, fields[0]) != NULL) {
        fprintf(stderr, "WARNING: %s line %d repeats copy ID %s, skipped\n", path, line_number, fields[0]);
        return false;
    }

    copy_field(copy.copy_id, sizeof(copy.copy_id), fields[0]);
    copy_field(copy.book_id, sizeof(copy.book_id), fields[1]);
    copy.state = COPY_AVAILABLE;
    registry->copies[registry->copy_count++] = copy;
    return true;
}

/*
 * Bad lines are skipped with a warning so one typo does not stop the whole
 * library, but a file that is missing or has no valid lines is an error.
 */
static bool load_file(Registry *registry, const char *path, const char *kind, int *count, int max_count,
                      LineHandler add_line)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "ERROR: cannot open %s (%s)\n", path, strerror(errno));
        return false;
    }

    char line[LINE_SIZE];
    int line_number = 0;
    *count = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        trim_whitespace(line);
        if (line[0] == '\0') {
            continue;
        }
        if (*count == max_count) {
            fprintf(stderr, "WARNING: %s has more than %d %s, the rest are ignored\n", path, max_count, kind);
            break;
        }

        char *fields[FIELDS_PER_LINE];
        if (!split_record(line, fields)) {
            fprintf(stderr, "WARNING: %s line %d is malformed, skipped\n", path, line_number);
            continue;
        }
        add_line(registry, fields, path, line_number);
    }
    fclose(file);

    if (*count == 0) {
        fprintf(stderr, "ERROR: %s is empty or has no valid %s\n", path, kind);
        return false;
    }
    return true;
}

bool load_registry(Registry *registry, const char *books_path, const char *members_path, const char *copies_path)
{
    bool books_loaded = load_file(registry, books_path, "books", &registry->book_count, MAX_BOOKS, add_book_line);
    bool members_loaded = load_file(registry, members_path, "members", &registry->member_count, MAX_MEMBERS,
                                    add_member_line);
    bool copies_loaded = books_loaded
                         && load_file(registry, copies_path, "copies", &registry->copy_count, MAX_COPIES,
                                      add_copy_line);
    return books_loaded && members_loaded && copies_loaded;
}

const Book *find_book(const Registry *registry, const char *book_id)
{
    for (int i = 0; i < registry->book_count; i++) {
        if (strcmp(registry->books[i].book_id, book_id) == 0) {
            return &registry->books[i];
        }
    }
    return NULL;
}

const Member *find_member(const Registry *registry, const char *member_id)
{
    for (int i = 0; i < registry->member_count; i++) {
        if (strcmp(registry->members[i].member_id, member_id) == 0) {
            return &registry->members[i];
        }
    }
    return NULL;
}

Copy *find_copy(Registry *registry, const char *copy_id)
{
    for (int i = 0; i < registry->copy_count; i++) {
        if (strcmp(registry->copies[i].copy_id, copy_id) == 0) {
            return &registry->copies[i];
        }
    }
    return NULL;
}

int count_copies(const Registry *registry, const char *book_id)
{
    int count = 0;
    for (int i = 0; i < registry->copy_count; i++) {
        if (strcmp(registry->copies[i].book_id, book_id) == 0) {
            count++;
        }
    }
    return count;
}

bool add_book(Registry *registry, const char *path, const Book *book)
{
    if (registry->book_count == MAX_BOOKS) {
        printf("ERROR: the registry is full (%d books)\n", MAX_BOOKS);
        return false;
    }

    char line[LINE_SIZE];
    snprintf(line, sizeof(line), "%s,%s,%s", book->book_id, book->title, book->author);
    if (!append_line_to_file(path, line)) {
        return false;
    }
    registry->books[registry->book_count++] = *book;
    return true;
}

bool add_member(Registry *registry, const char *path, const Member *member)
{
    if (registry->member_count == MAX_MEMBERS) {
        printf("ERROR: the registry is full (%d members)\n", MAX_MEMBERS);
        return false;
    }

    char line[LINE_SIZE];
    snprintf(line, sizeof(line), "%s,%s,%s", member->member_id, member->full_name, member->course_code);
    if (!append_line_to_file(path, line)) {
        return false;
    }
    registry->members[registry->member_count++] = *member;
    return true;
}

/* The new copy is numbered one above the highest copy number its book already has. */
static int next_copy_number(const Registry *registry, const char *book_id)
{
    int highest = 0;
    for (int i = 0; i < registry->copy_count; i++) {
        const Copy *copy = &registry->copies[i];
        if (strcmp(copy->book_id, book_id) == 0) {
            int number = atoi(strrchr(copy->copy_id, '-') + 1);
            if (number > highest) {
                highest = number;
            }
        }
    }
    return highest + 1;
}

bool add_copy(Registry *registry, const char *path, const char *book_id, CopyCondition condition, Copy **created)
{
    if (registry->copy_count == MAX_COPIES) {
        printf("ERROR: the registry is full (%d copies)\n", MAX_COPIES);
        return false;
    }

    int number = next_copy_number(registry, book_id);
    if (number > MAX_COPY_NUMBER) {
        printf("ERROR: %s already has the maximum number of copies\n", book_id);
        return false;
    }

    Copy copy = {0};
    snprintf(copy.copy_id, sizeof(copy.copy_id), "%s-%d", book_id, number);
    copy_field(copy.book_id, sizeof(copy.book_id), book_id);
    copy.condition = condition;
    copy.state = COPY_AVAILABLE;

    char line[LINE_SIZE];
    snprintf(line, sizeof(line), "%s,%s,%s", copy.copy_id, copy.book_id, condition_name(condition));
    if (!append_line_to_file(path, line)) {
        return false;
    }
    registry->copies[registry->copy_count] = copy;
    *created = &registry->copies[registry->copy_count];
    registry->copy_count++;
    return true;
}
