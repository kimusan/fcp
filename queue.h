/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_QUEUE_H
#define FCP_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* Queue item: a file to copy */
typedef struct {
    char *src;
    char *dst;
    uint64_t size;
} fcp_queue_item_t;

/* Thread-safe file queue */
typedef struct {
    fcp_queue_item_t *items;
    int count;
    int capacity;
    int head;
    int tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    bool done; /* Set when no more items will be added */
} fcp_queue_t;

/* Initialize queue */
void fcp_queue_init(fcp_queue_t *queue, int initial_capacity);

/* Add item to queue (blocks if full) */
int fcp_queue_push(fcp_queue_t *queue, const char *src, const char *dst, uint64_t size);

/* Remove item from queue (blocks if empty) */
int fcp_queue_pop(fcp_queue_t *queue, fcp_queue_item_t *item);

/* Mark queue as done (no more items will be added) */
void fcp_queue_mark_done(fcp_queue_t *queue);

/* Check if queue is empty and done */
bool fcp_queue_is_empty(fcp_queue_t *queue);

/* Cleanup queue */
void fcp_queue_cleanup(fcp_queue_t *queue);

#endif /* FCP_QUEUE_H */