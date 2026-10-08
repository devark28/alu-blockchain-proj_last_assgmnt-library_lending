#ifndef RECORD_H
#define RECORD_H

#include "crypto.h"
#include "registry.h"

#include <stdbool.h>
#include <time.h>

#define CONDITION_TEXT_SIZE 8
#define REASON_SIZE 24
#define RECORD_LINE_SIZE 1024
#define PROBLEM_SIZE 160

typedef enum {
    ACTION_BORROWED,
    ACTION_RETURNED,
    ACTION_RESERVED,
    ACTION_RENEWED,
    ACTION_OVERDUE,
    ACTION_LOST,
    ACTION_REGISTERED,
    ACTION_TRANSFER,
    ACTION_FINE_PAID
} RecordAction;

/*
 * One signed entry in a block. The first group of fields is the lending
 * record; condition and reason_code describe returns and lost copies,
 * recipient_id and amount describe token payments, and public_key_hex is
 * the key a REGISTERED record puts on the chain. Unused fields are empty.
 *
 * record_id is the SHA-256 of every field except the signature, so the ID
 * cannot be changed by re-encoding a signature.
 */
typedef struct {
    char record_id[HASH_HEX_SIZE];
    RecordAction action;
    char signer_id[ID_SIZE];
    int nonce;
    time_t timestamp;
    char book_id[ID_SIZE];
    char copy_id[ID_SIZE];
    char book_title[TITLE_SIZE];
    char member_id[ID_SIZE];
    char member_name[NAME_SIZE];
    time_t due_date;
    int fine_amount;
    char condition[CONDITION_TEXT_SIZE];
    char reason_code[REASON_SIZE];
    char recipient_id[ID_SIZE];
    int amount;
    char public_key_hex[PUBLIC_KEY_HEX_SIZE];
    unsigned char signature[MAX_SIGNATURE_SIZE];
} Record;

const char *action_name(RecordAction action);
bool parse_action(const char *text, RecordAction *action);
bool action_needs_librarian(RecordAction action);

void record_payload(const Record *record, char *buffer, size_t size);
void compute_record_id(const Record *record, char output[HASH_HEX_SIZE]);
void compute_record_leaf(const Record *record, char output[HASH_HEX_SIZE]);
bool sign_record(Record *record, EVP_PKEY *private_key);
bool record_signature_is_valid(const Record *record, EVP_PKEY *public_key);
bool record_signer_is_authorized(const Record *record);

void format_record_line(const Record *record, char *buffer, size_t size);
bool parse_record_line(char *line, Record *record);

void print_record(const Record *record);

#endif
