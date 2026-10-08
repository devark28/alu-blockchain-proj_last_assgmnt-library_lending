#ifndef POOL_H
#define POOL_H

#include "record.h"

#include <stdbool.h>
#include <time.h>

#define PRIORITY_OVERDUE 0
#define PRIORITY_NORMAL 1
#define FLAG_REASON_SIZE 200
#define MIN_ID_PREFIX 6

typedef enum {
    REQUEST_PENDING,
    REQUEST_SUSPICIOUS,
    REQUEST_APPROVED
} RequestStatus;

/*
 * A signed record waiting to be mined. The request ID is the record's
 * record_id. priority puts overdue-related requests first, and order_time
 * (the member's reservation date, or else the submission time) breaks ties
 * so the member who has waited longest is served first.
 */
typedef struct {
    Record record;
    int priority;
    time_t order_time;
    RequestStatus status;
    char flag_reason[FLAG_REASON_SIZE];
} PendingRequest;

/* Kept in submission order; the mining order is a sorted view of it. */
typedef struct {
    PendingRequest *items;
    int count;
    int capacity;
} Pool;

bool pool_add(Pool *pool, const PendingRequest *request);
void pool_remove(Pool *pool, int index);
int find_request(const Pool *pool, const char *id_or_prefix, bool suspicious);
void sort_pool_order(const Pool *pool, int order[]);
void pool_free(Pool *pool);

const char *request_status_name(RequestStatus status);
void print_pool(const Pool *pool);

bool save_pool(const char *path, const Pool *pool);
bool load_pool(const char *path, Pool *pool);

#endif
