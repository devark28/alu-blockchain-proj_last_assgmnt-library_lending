#ifndef RESERVATION_H
#define RESERVATION_H

#include "registry.h"

#include <stdbool.h>
#include <time.h>

typedef struct ReservationNode {
    char member_id[ID_SIZE];
    time_t reservation_date;
    struct ReservationNode *next;
} ReservationNode;

/* First in, first out: members join at the tail and are served from the head. */
typedef struct {
    char book_id[ID_SIZE];
    ReservationNode *head;
    ReservationNode *tail;
    int length;
} ReservationQueue;

typedef struct {
    ReservationQueue *queues;
    int count;
    int capacity;
} ReservationList;

ReservationQueue *find_queue(const ReservationList *list, const char *book_id);
bool enqueue_reservation(ReservationList *list, const char *book_id, const char *member_id, time_t reservation_date);
bool dequeue_reservation(ReservationList *list, const char *book_id, char member_id[ID_SIZE]);
bool remove_reservation(ReservationList *list, const char *book_id, const char *member_id);
int queue_position(const ReservationList *list, const char *book_id, const char *member_id);
const ReservationNode *find_reservation(const ReservationList *list, const char *book_id, const char *member_id);
bool clone_reservations(const ReservationList *source, ReservationList *copy);
void reservations_free(ReservationList *list);

#endif
