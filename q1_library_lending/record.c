#include "record.h"
#include "utils.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_SIZE 768
#define RECORD_TAG "R"
#define FIELDS_PER_RECORD_LINE 19
#define MAX_TIME_VALUE 32503680000LL

static const char *ACTION_NAMES[] = {
    "BORROWED", "RETURNED", "RESERVED", "RENEWED", "OVERDUE", "LOST", "REGISTERED", "TRANSFER", "FINE_PAID"
};

static const int ACTION_COUNT = (int)(sizeof(ACTION_NAMES) / sizeof(ACTION_NAMES[0]));

const char *action_name(RecordAction action)
{
    return ACTION_NAMES[action];
}

bool parse_action(const char *text, RecordAction *action)
{
    for (int i = 0; i < ACTION_COUNT; i++) {
        if (strcmp(text, ACTION_NAMES[i]) == 0) {
            *action = (RecordAction)i;
            return true;
        }
    }
    return false;
}

/*
 * Returns, overdue flags, lost copies and new identities are facts the
 * library observes, so the librarian signs them. Everything else is a
 * member's own request and must be signed by that member.
 */
bool action_needs_librarian(RecordAction action)
{
    return action == ACTION_RETURNED || action == ACTION_OVERDUE || action == ACTION_LOST
           || action == ACTION_REGISTERED;
}

/* Every field except the record ID and the signature, joined with '|'. */
void record_payload(const Record *record, char *buffer, size_t size)
{
    snprintf(buffer, size, "%s|%s|%d|%lld|%s|%s|%s|%s|%s|%lld|%d|%s|%s|%s|%d|%s",
             action_name(record->action), record->signer_id, record->nonce, (long long)record->timestamp,
             record->book_id, record->copy_id, record->book_title, record->member_id, record->member_name,
             (long long)record->due_date, record->fine_amount, record->condition, record->reason_code,
             record->recipient_id, record->amount, record->public_key_hex);
}

void compute_record_id(const Record *record, char output[HASH_HEX_SIZE])
{
    char payload[PAYLOAD_SIZE];
    record_payload(record, payload, sizeof(payload));
    sha256_hex(payload, output);
}

static void signature_to_hex(const Record *record, char hex[SIGNATURE_HEX_SIZE])
{
    bytes_to_hex(record->signature, signature_length(record->signature), hex);
}

/*
 * A Merkle leaf covers the payload and the signature, so editing any field
 * or replacing the signature changes the block's Merkle root.
 */
void compute_record_leaf(const Record *record, char output[HASH_HEX_SIZE])
{
    char payload[PAYLOAD_SIZE];
    char signature_hex[SIGNATURE_HEX_SIZE];
    char leaf_input[PAYLOAD_SIZE + SIGNATURE_HEX_SIZE + 1];

    record_payload(record, payload, sizeof(payload));
    signature_to_hex(record, signature_hex);
    snprintf(leaf_input, sizeof(leaf_input), "%s|%s", payload, signature_hex);
    sha256_hex(leaf_input, output);
}

bool sign_record(Record *record, EVP_PKEY *private_key)
{
    char payload[PAYLOAD_SIZE];
    record_payload(record, payload, sizeof(payload));
    if (!sign_data(private_key, payload, record->signature)) {
        printf("ERROR: could not sign the %s record\n", action_name(record->action));
        return false;
    }
    sha256_hex(payload, record->record_id);
    return true;
}

bool record_signature_is_valid(const Record *record, EVP_PKEY *public_key)
{
    char payload[PAYLOAD_SIZE];
    record_payload(record, payload, sizeof(payload));
    return verify_signature(public_key, payload, record->signature);
}

bool record_signer_is_authorized(const Record *record)
{
    if (action_needs_librarian(record->action)) {
        return strcmp(record->signer_id, LIBRARIAN_ID) == 0;
    }
    return strcmp(record->signer_id, record->member_id) == 0;
}

void format_record_line(const Record *record, char *buffer, size_t size)
{
    char payload[PAYLOAD_SIZE];
    char signature_hex[SIGNATURE_HEX_SIZE];
    record_payload(record, payload, sizeof(payload));
    signature_to_hex(record, signature_hex);
    snprintf(buffer, size, "%s|%s|%s|%s", RECORD_TAG, record->record_id, payload, signature_hex);
}

static bool parse_int_field(const char *text, int *value)
{
    long long number;
    if (!parse_number(text, INT_MIN, INT_MAX, &number)) {
        return false;
    }
    *value = (int)number;
    return true;
}

static bool parse_time_field(const char *text, time_t *value)
{
    long long number;
    if (!parse_number(text, 0, MAX_TIME_VALUE, &number)) {
        return false;
    }
    *value = (time_t)number;
    return true;
}

/*
 * Parses one R line written by format_record_line. Every field is length
 * and type checked, because the file can be edited by hand.
 */
bool parse_record_line(char *line, Record *record)
{
    char *fields[FIELDS_PER_RECORD_LINE];
    if (split_line(line, '|', fields, FIELDS_PER_RECORD_LINE) != FIELDS_PER_RECORD_LINE
        || strcmp(fields[0], RECORD_TAG) != 0) {
        return false;
    }

    memset(record, 0, sizeof(*record));
    const char *signature_hex = fields[18];
    int signature_bytes = hex_to_bytes(signature_hex, record->signature, MAX_SIGNATURE_SIZE);

    return copy_field(record->record_id, sizeof(record->record_id), fields[1])
           && parse_action(fields[2], &record->action)
           && copy_field(record->signer_id, sizeof(record->signer_id), fields[3])
           && parse_int_field(fields[4], &record->nonce)
           && parse_time_field(fields[5], &record->timestamp)
           && copy_field(record->book_id, sizeof(record->book_id), fields[6])
           && copy_field(record->copy_id, sizeof(record->copy_id), fields[7])
           && copy_field(record->book_title, sizeof(record->book_title), fields[8])
           && copy_field(record->member_id, sizeof(record->member_id), fields[9])
           && copy_field(record->member_name, sizeof(record->member_name), fields[10])
           && parse_time_field(fields[11], &record->due_date)
           && parse_int_field(fields[12], &record->fine_amount)
           && copy_field(record->condition, sizeof(record->condition), fields[13])
           && copy_field(record->reason_code, sizeof(record->reason_code), fields[14])
           && copy_field(record->recipient_id, sizeof(record->recipient_id), fields[15])
           && parse_int_field(fields[16], &record->amount)
           && copy_field(record->public_key_hex, sizeof(record->public_key_hex), fields[17])
           && signature_bytes >= 0;
}

void print_record(const Record *record)
{
    char created[TIME_TEXT_SIZE];
    format_time(record->timestamp, created);

    printf("    %-10s %.16s...  signer %s, nonce %d, %s\n", action_name(record->action), record->record_id,
           record->signer_id, record->nonce, created);

    if (record->action == ACTION_REGISTERED) {
        printf("               identity %s, public key %.24s...\n", record->member_id, record->public_key_hex);
        return;
    }
    if (record->action == ACTION_TRANSFER || record->action == ACTION_FINE_PAID) {
        printf("               %s pays %d to %s\n", record->member_id, record->amount, record->recipient_id);
        return;
    }

    char due[TIME_TEXT_SIZE];
    format_time(record->due_date, due);
    printf("               %s (%s) %s, %s (%s), due %s\n", record->copy_id, record->book_id, record->book_title,
           record->member_id, record->member_name, due);
    if (record->fine_amount > 0 || record->condition[0] != '\0' || record->reason_code[0] != '\0') {
        printf("               fine %d, condition %s, reason %s\n", record->fine_amount,
               record->condition[0] != '\0' ? record->condition : "-",
               record->reason_code[0] != '\0' ? record->reason_code : "-");
    }
}
