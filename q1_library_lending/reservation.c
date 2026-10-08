#include "reservation.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

ReservationQueue *find_queue(const ReservationList *list, const char *book_id)
{
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->queues[i].book_id, book_id) == 0) {
            return &list->queues[i];
        }
    }
    return NULL;
}

static ReservationQueue *find_or_add_queue(ReservationList *list, const char *book_id)
{
    ReservationQueue *queue = find_queue(list, book_id);
    if (queue != NULL) {
        return queue;
    }

    if (list->count == list->capacity) {
        int new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        ReservationQueue *grown = realloc(list->queues, (size_t)new_capacity * sizeof(ReservationQueue));
        if (grown == NULL) {
            return NULL;
        }
        list->queues = grown;
        list->capacity = new_capacity;
    }
    queue = &list->queues[list->count++];
    memset(queue, 0, sizeof(*queue));
    copy_field(queue->book_id, sizeof(queue->book_id), book_id);
    return queue;
}

bool enqueue_reservation(ReservationList *list, const char *book_id, const char *member_id, time_t reservation_date)
{
    ReservationQueue *queue = find_or_add_queue(list, book_id);
    ReservationNode *node = malloc(sizeof(ReservationNode));
    if (queue == NULL || node == NULL) {
        free(node);
        return false;
    }

    copy_field(node->member_id, sizeof(node->member_id), member_id);
    node->reservation_date = reservation_date;
    node->next = NULL;
    if (queue->tail == NULL) {
        queue->head = node;
    } else {
        queue->tail->next = node;
    }
    queue->tail = node;
    queue->length++;
    return true;
}

/* Removes the member at the head of the queue and returns their ID. */
bool dequeue_reservation(ReservationList *list, const char *book_id, char member_id[ID_SIZE])
{
    ReservationQueue *queue = find_queue(list, book_id);
    if (queue == NULL || queue->head == NULL) {
        return false;
    }
    copy_field(member_id, ID_SIZE, queue->head->member_id);
    return remove_reservation(list, book_id, member_id);
}

bool remove_reservation(ReservationList *list, const char *book_id, const char *member_id)
{
    ReservationQueue *queue = find_queue(list, book_id);
    if (queue == NULL) {
        return false;
    }

    ReservationNode *previous = NULL;
    for (ReservationNode *node = queue->head; node != NULL; node = node->next) {
        if (strcmp(node->member_id, member_id) == 0) {
            if (previous == NULL) {
                queue->head = node->next;
            } else {
                previous->next = node->next;
            }
            if (queue->tail == node) {
                queue->tail = previous;
            }
            free(node);
            queue->length--;
            return true;
        }
        previous = node;
    }
    return false;
}

/* 1 for the head of the queue, 0 when the member is not in it. */
int queue_position(const ReservationList *list, const char *book_id, const char *member_id)
{
    const ReservationQueue *queue = find_queue(list, book_id);
    int position = 1;
    for (const ReservationNode *node = queue != NULL ? queue->head : NULL; node != NULL; node = node->next) {
        if (strcmp(node->member_id, member_id) == 0) {
            return position;
        }
        position++;
    }
    return 0;
}

const ReservationNode *find_reservation(const ReservationList *list, const char *book_id, const char *member_id)
{
    const ReservationQueue *queue = find_queue(list, book_id);
    for (const ReservationNode *node = queue != NULL ? queue->head : NULL; node != NULL; node = node->next) {
        if (strcmp(node->member_id, member_id) == 0) {
            return node;
        }
    }
    return NULL;
}

bool clone_reservations(const ReservationList *source, ReservationList *copy)
{
    memset(copy, 0, sizeof(*copy));
    for (int i = 0; i < source->count; i++) {
        const ReservationQueue *queue = &source->queues[i];
        for (const ReservationNode *node = queue->head; node != NULL; node = node->next) {
            if (!enqueue_reservation(copy, queue->book_id, node->member_id, node->reservation_date)) {
                reservations_free(copy);
                return false;
            }
        }
    }
    return true;
}

void reservations_free(ReservationList *list)
{
    for (int i = 0; i < list->count; i++) {
        ReservationNode *node = list->queues[i].head;
        while (node != NULL) {
            ReservationNode *next = node->next;
            free(node);
            node = next;
        }
    }
    free(list->queues);
    list->queues = NULL;
    list->count = 0;
    list->capacity = 0;
}
