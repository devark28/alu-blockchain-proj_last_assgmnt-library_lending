#include "pool.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUEST_TAG "P"
#define FIELDS_PER_REQUEST_LINE 5
#define FILE_LINE_SIZE 1100

static const char *STATUS_NAMES[] = {"PENDING", "SUSPICIOUS", "APPROVED"};

const char *request_status_name(RequestStatus status)
{
    return STATUS_NAMES[status];
}

static bool parse_status(const char *text, RequestStatus *status)
{
    for (int i = REQUEST_PENDING; i <= REQUEST_APPROVED; i++) {
        if (strcmp(text, STATUS_NAMES[i]) == 0) {
            *status = (RequestStatus)i;
            return true;
        }
    }
    return false;
}

bool pool_add(Pool *pool, const PendingRequest *request)
{
    if (pool->count == pool->capacity) {
        int new_capacity = pool->capacity == 0 ? 16 : pool->capacity * 2;
        PendingRequest *grown = realloc(pool->items, (size_t)new_capacity * sizeof(PendingRequest));
        if (grown == NULL) {
            printf("ERROR: out of memory while growing the pending pool\n");
            return false;
        }
        pool->items = grown;
        pool->capacity = new_capacity;
    }
    pool->items[pool->count++] = *request;
    return true;
}

void pool_remove(Pool *pool, int index)
{
    memmove(&pool->items[index], &pool->items[index + 1],
            (size_t)(pool->count - index - 1) * sizeof(PendingRequest));
    pool->count--;
}

/*
 * Accepts a full request ID or a unique prefix of at least MIN_ID_PREFIX
 * characters, as shown by pool_view, among requests that are (or are not)
 * flagged SUSPICIOUS. A replayed request has the same ID as its original,
 * so the flag is what tells the two apart. When every match has the very
 * same ID (an original and its replay, both flagged), the newest is chosen.
 * Returns -1 when nothing matches or the prefix fits different requests.
 */
int find_request(const Pool *pool, const char *id_or_prefix, bool suspicious)
{
    size_t length = strlen(id_or_prefix);
    int match = -1;
    if (length < MIN_ID_PREFIX) {
        return -1;
    }
    for (int i = 0; i < pool->count; i++) {
        const PendingRequest *request = &pool->items[i];
        if ((request->status == REQUEST_SUSPICIOUS) != suspicious
            || strncmp(request->record.record_id, id_or_prefix, length) != 0) {
            continue;
        }
        if (match >= 0 && strcmp(pool->items[match].record.record_id, request->record.record_id) != 0) {
            return -1;
        }
        match = i;
    }
    return match;
}

/*
 * True when request a should be mined before request b: priority first,
 * then order_time ascending, then submission order, which also keeps one
 * member's requests in nonce order.
 */
static bool comes_before(const Pool *pool, int a, int b)
{
    const PendingRequest *first = &pool->items[a];
    const PendingRequest *second = &pool->items[b];
    if (first->priority != second->priority) {
        return first->priority < second->priority;
    }
    if (first->order_time != second->order_time) {
        return first->order_time < second->order_time;
    }
    return a < b;
}

/* Fills order with the pool's indices in mining order, using an insertion sort. */
void sort_pool_order(const Pool *pool, int order[])
{
    for (int i = 0; i < pool->count; i++) {
        int index = i;
        int position = i;
        while (position > 0 && comes_before(pool, index, order[position - 1])) {
            order[position] = order[position - 1];
            position--;
        }
        order[position] = index;
    }
}

void pool_free(Pool *pool)
{
    free(pool->items);
    pool->items = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

static void print_request_subject(const Record *record)
{
    if (record->action == ACTION_REGISTERED) {
        printf("identity for %s", record->member_id);
    } else if (record->action == ACTION_RESERVED) {
        printf("%s for %s", record->book_id, record->member_id);
    } else if (record->action == ACTION_TRANSFER || record->action == ACTION_FINE_PAID) {
        printf("%s pays %d to %s", record->member_id, record->amount, record->recipient_id);
    } else {
        printf("%s for %s", record->copy_id, record->member_id);
    }
}

void print_pool(const Pool *pool)
{
    if (pool->count == 0) {
        printf("The pending pool is empty.\n");
        return;
    }

    int *order = malloc((size_t)pool->count * sizeof(int));
    if (order == NULL) {
        return;
    }
    sort_pool_order(pool, order);

    printf("Pending pool: %d request%s, in the order a miner takes them\n", pool->count, pool->count == 1 ? "" : "s");
    printf("  %-3s %-12s %-10s %-10s %-5s %-6s %-19s %s\n", "#", "request_id", "status", "action", "prio",
           "nonce", "order time", "subject");
    for (int i = 0; i < pool->count; i++) {
        const PendingRequest *request = &pool->items[order[i]];
        char order_time[TIME_TEXT_SIZE];
        format_time(request->order_time, order_time);
        printf("  %-3d %.12s %-10s %-10s %-5s %-6d %-19s ", i + 1, request->record.record_id,
               request_status_name(request->status), action_name(request->record.action),
               request->priority == PRIORITY_OVERDUE ? "high" : "normal", request->record.nonce, order_time);
        print_request_subject(&request->record);
        printf("\n");
        if (request->flag_reason[0] != '\0') {
            printf("      flagged: %s\n", request->flag_reason);
        }
    }
    free(order);
}

bool save_pool(const char *path, const Pool *pool)
{
    char temporary_path[256];
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path);

    FILE *file = fopen(temporary_path, "w");
    if (file == NULL) {
        printf("ERROR: cannot write %s (%s)\n", temporary_path, strerror(errno));
        return false;
    }

    char line[FILE_LINE_SIZE];
    for (int i = 0; i < pool->count; i++) {
        const PendingRequest *request = &pool->items[i];
        fprintf(file, "%s|%s|%d|%lld|%s\n", REQUEST_TAG, request_status_name(request->status), request->priority,
                (long long)request->order_time, request->flag_reason);
        format_record_line(&request->record, line, sizeof(line));
        fprintf(file, "%s\n", line);
    }

    if (fclose(file) != 0 || rename(temporary_path, path) != 0) {
        printf("ERROR: cannot save %s (%s)\n", path, strerror(errno));
        remove(temporary_path);
        return false;
    }
    return true;
}

static bool parse_request_line(char *line, PendingRequest *request)
{
    char *fields[FIELDS_PER_REQUEST_LINE];
    long long priority, order_time;
    memset(request, 0, sizeof(*request));

    if (split_line(line, '|', fields, FIELDS_PER_REQUEST_LINE) != FIELDS_PER_REQUEST_LINE
        || strcmp(fields[0], REQUEST_TAG) != 0
        || !parse_status(fields[1], &request->status)
        || !parse_number(fields[2], PRIORITY_OVERDUE, PRIORITY_NORMAL, &priority)
        || !parse_number(fields[3], 0, MAX_TIME_VALUE, &order_time)
        || !copy_field(request->flag_reason, sizeof(request->flag_reason), fields[4])) {
        return false;
    }
    request->priority = (int)priority;
    request->order_time = (time_t)order_time;
    return true;
}

/*
 * A missing pool file just means an empty pool. Requests are re-checked
 * against the chain after loading, so an edited line is caught there.
 */
bool load_pool(const char *path, Pool *pool)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return errno == ENOENT;
    }

    char line[FILE_LINE_SIZE];
    int line_number = 0;
    bool loaded = true;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        PendingRequest request;
        bool parsed = parse_request_line(line, &request) && fgets(line, sizeof(line), file) != NULL;
        if (parsed) {
            line_number++;
            line[strcspn(line, "\r\n")] = '\0';
            parsed = parse_record_line(line, &request.record);
        }
        if (!parsed) {
            printf("ERROR: %s line %d is not a valid pending request\n", path, line_number);
            loaded = false;
            break;
        }
        if (!pool_add(pool, &request)) {
            loaded = false;
            break;
        }
    }
    fclose(file);

    if (!loaded) {
        pool_free(pool);
    }
    return loaded;
}
