#include "commands.h"
#include "inventory.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

static const Book *require_book(const Registry *registry, const char *book_id)
{
    if (!is_valid_book_id(book_id)) {
        printf("ERROR: %s is not a valid book ID (expected BK followed by 3 to 6 digits)\n", book_id);
        return NULL;
    }
    const Book *book = find_book(registry, book_id);
    if (book == NULL) {
        printf("ERROR: Book not found: %s\n", book_id);
    }
    return book;
}

static const Member *require_member(const Registry *registry, const char *member_id)
{
    if (!is_valid_member_id(member_id)) {
        printf("ERROR: %s is not a valid member ID (expected ALU followed by 3 to 6 digits)\n", member_id);
        return NULL;
    }
    const Member *member = find_member(registry, member_id);
    if (member == NULL) {
        printf("ERROR: Member not found: %s\n", member_id);
    }
    return member;
}

static void print_member_row(const Member *member)
{
    printf("  %-8s ", member->member_id);
    print_padded(member->full_name, 24);
    printf(" %s\n", member->course_code);
}

bool command_view_member(LibrarySystem *library, int arg_count, char *args[])
{
    const Registry *registry = &library->registry;

    if (arg_count == 0) {
        printf("Members (%d)\n", registry->member_count);
        for (int i = 0; i < registry->member_count; i++) {
            print_member_row(&registry->members[i]);
        }
        return true;
    }

    const Member *member = require_member(registry, args[0]);
    if (member == NULL) {
        return false;
    }
    printf("Member %s\n", member->member_id);
    printf("  name:   %s\n", member->full_name);
    printf("  course: %s\n", member->course_code);
    printf("  copies held:\n");

    bool holds_any = false;
    for (int i = 0; i < registry->copy_count; i++) {
        const Copy *copy = &registry->copies[i];
        if (strcmp(copy->current_holder_id, member->member_id) == 0) {
            if (!holds_any) {
                print_copy_header();
                holds_any = true;
            }
            print_copy_row(copy);
        }
    }
    if (!holds_any) {
        printf("  none\n");
    }
    return true;
}

bool command_register_book(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    Registry *registry = &library->registry;
    Book book;

    if (!is_valid_book_id(args[0])) {
        printf("ERROR: %s is not a valid book ID (expected BK followed by 3 to 6 digits)\n", args[0]);
        return false;
    }
    if (find_book(registry, args[0]) != NULL) {
        printf("ERROR: book %s is already registered\n", args[0]);
        return false;
    }
    if (!is_valid_text_field(args[1]) || !is_valid_text_field(args[2])) {
        printf("ERROR: title and author must not be empty or contain commas, '|' or quotes\n");
        return false;
    }
    if (!copy_field(book.book_id, sizeof(book.book_id), args[0])
        || !copy_field(book.title, sizeof(book.title), args[1])
        || !copy_field(book.author, sizeof(book.author), args[2])) {
        printf("ERROR: title is limited to %d characters and author to %d\n", TITLE_SIZE - 1, AUTHOR_SIZE - 1);
        return false;
    }

    if (!add_book(registry, BOOKS_FILE, &book)) {
        return false;
    }
    printf("Registered %s: %s by %s. Add copies with add_copy %s <condition>\n",
           book.book_id, book.title, book.author, book.book_id);
    return true;
}

bool command_view_book(LibrarySystem *library, int arg_count, char *args[])
{
    Registry *registry = &library->registry;

    if (arg_count == 0) {
        printf("Books (%d)\n", registry->book_count);
        for (int i = 0; i < registry->book_count; i++) {
            print_book_summary(registry, &registry->books[i]);
        }
        return true;
    }

    const Book *book = require_book(registry, args[0]);
    if (book == NULL) {
        return false;
    }
    printf("Book %s\n", book->book_id);
    printf("  title:  %s\n", book->title);
    printf("  author: %s\n", book->author);
    printf("  copies: %d of %d available\n", count_copies_in_state(registry, book->book_id, COPY_AVAILABLE),
           count_copies(registry, book->book_id));

    print_copy_header();
    for (int i = 0; i < registry->copy_count; i++) {
        if (strcmp(registry->copies[i].book_id, book->book_id) == 0) {
            print_copy_row(&registry->copies[i]);
        }
    }
    return true;
}

bool command_add_copy(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    Registry *registry = &library->registry;

    const Book *book = require_book(registry, args[0]);
    if (book == NULL) {
        return false;
    }
    CopyCondition condition;
    if (!parse_condition(args[1], &condition) || condition == CONDITION_LOST) {
        printf("ERROR: condition must be NEW, GOOD or WORN\n");
        return false;
    }

    Copy *copy;
    if (!add_copy(registry, COPIES_FILE, book->book_id, condition, &copy)) {
        return false;
    }
    printf("Added copy %s of %s (%s), condition %s\n", copy->copy_id, book->book_id, book->title,
           condition_name(condition));
    return true;
}

bool command_view_copy(LibrarySystem *library, int arg_count, char *args[])
{
    (void)arg_count;
    Registry *registry = &library->registry;

    if (!is_valid_copy_id(args[0])) {
        printf("ERROR: %s is not a valid copy ID (expected a book ID, a dash and a number, such as BK001-2)\n",
               args[0]);
        return false;
    }
    const Copy *copy = find_copy(registry, args[0]);
    if (copy == NULL) {
        printf("ERROR: Copy not found: %s\n", args[0]);
        return false;
    }

    const Book *book = find_book(registry, copy->book_id);
    printf("Copy %s of %s (%s)\n", copy->copy_id, book->book_id, book->title);
    print_copy_header();
    print_copy_row(copy);
    return true;
}
