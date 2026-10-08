#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdbool.h>
#include <time.h>

#define MAX_BOOKS 200
#define MAX_MEMBERS 200
#define MAX_COPIES 600
#define ID_SIZE 20
#define TITLE_SIZE 80
#define AUTHOR_SIZE 50
#define NAME_SIZE 50
#define COURSE_SIZE 10

typedef enum {
    CONDITION_NEW,
    CONDITION_GOOD,
    CONDITION_WORN,
    CONDITION_LOST
} CopyCondition;

typedef enum {
    COPY_AVAILABLE,
    COPY_BORROWED,
    COPY_RESERVED,
    COPY_LOST
} CopyState;

typedef struct {
    char book_id[ID_SIZE];
    char title[TITLE_SIZE];
    char author[AUTHOR_SIZE];
} Book;

typedef struct {
    char member_id[ID_SIZE];
    char full_name[NAME_SIZE];
    char course_code[COURSE_SIZE];
} Member;

/*
 * copies.txt only provides copy_id, book_id and the starting condition.
 * The other fields describe the copy's current lending state.
 */
typedef struct {
    char copy_id[ID_SIZE];
    char book_id[ID_SIZE];
    CopyCondition condition;
    CopyState state;
    char current_holder_id[ID_SIZE];
    time_t due_date;
    int renewal_count;
} Copy;

typedef struct {
    Book books[MAX_BOOKS];
    int book_count;
    Member members[MAX_MEMBERS];
    int member_count;
    Copy copies[MAX_COPIES];
    int copy_count;
} Registry;

bool load_registry(Registry *registry, const char *books_path, const char *members_path, const char *copies_path);

const Book *find_book(const Registry *registry, const char *book_id);
const Member *find_member(const Registry *registry, const char *member_id);
Copy *find_copy(Registry *registry, const char *copy_id);
int count_copies(const Registry *registry, const char *book_id);

bool is_valid_book_id(const char *id);
bool is_valid_member_id(const char *id);
bool is_valid_copy_id(const char *id);
bool is_valid_text_field(const char *text);

bool add_book(Registry *registry, const char *path, const Book *book);
bool add_member(Registry *registry, const char *path, const Member *member);
bool add_copy(Registry *registry, const char *path, const char *book_id, CopyCondition condition, Copy **created);

const char *condition_name(CopyCondition condition);
bool parse_condition(const char *text, CopyCondition *condition);
const char *copy_state_name(CopyState state);

#endif
