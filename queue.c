/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "queue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define QUEUE_INITIAL_CAPACITY 64
#define QUEUE_MAX_CAPACITY 65536

void fcp_queue_init(fcp_queue_t *queue, int initial_capacity) {
    if (initial_capacity <= 0) {
        initial_capacity = QUEUE_INITIAL_CAPACITY;
    }

    queue->items = malloc(sizeof(fcp_queue_item_t) * initial_capacity);
    if (!queue->items) {
        perror("fcp: malloc");
        exit(1);
    }

    queue->count = 0;
    queue->capacity = initial_capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->done = false;

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond_not_empty, NULL);
    pthread_cond_init(&queue->cond_not_full, NULL);
}

int fcp_queue_push(fcp_queue_t *queue, const char *src, const char *dst, uint64_t size) {
    pthread_mutex_lock(&queue->mutex);

    /* Grow queue if full */
    if (queue->count >= queue->capacity) {
        if (queue->capacity >= QUEUE_MAX_CAPACITY) {
            pthread_mutex_unlock(&queue->mutex);
            fprintf(stderr, "fcp: queue full\n");
            return -1;
        }

        int new_capacity = queue->capacity * 2;
        fcp_queue_item_t *new_items = realloc(queue->items, sizeof(fcp_queue_item_t) * new_capacity);
        if (!new_items) {
            pthread_mutex_unlock(&queue->mutex);
            perror("fcp: realloc");
            return -1;
        }

        /* Rebuild array to handle circular buffer */
        fcp_queue_item_t *temp = malloc(sizeof(fcp_queue_item_t) * new_capacity);
        if (!temp) {
            free(new_items);
            pthread_mutex_unlock(&queue->mutex);
            perror("fcp: malloc");
            return -1;
        }

        for (int i = 0; i < queue->count; i++) {
            int idx = (queue->head + i) % queue->capacity;
            temp[i] = queue->items[idx];
        }

        free(queue->items);
        queue->items = new_items;
        memcpy(queue->items, temp, sizeof(fcp_queue_item_t) * queue->count);
        free(temp);

        queue->capacity = new_capacity;
        queue->head = 0;
        queue->tail = queue->count;
    }

    /* Add item */
    queue->items[queue->tail].src = strdup(src);
    queue->items[queue->tail].dst = strdup(dst);
    queue->items[queue->tail].size = size;

    if (!queue->items[queue->tail].src || !queue->items[queue->tail].dst) {
        free(queue->items[queue->tail].src);
        free(queue->items[queue->tail].dst);
        pthread_mutex_unlock(&queue->mutex);
        perror("fcp: strdup");
        return -1;
    }

    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    pthread_cond_signal(&queue->cond_not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

int fcp_queue_pop(fcp_queue_t *queue, fcp_queue_item_t *item) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->count == 0 && !queue->done) {
        pthread_cond_wait(&queue->cond_not_empty, &queue->mutex);
    }

    if (queue->count == 0 && queue->done) {
        pthread_mutex_unlock(&queue->mutex);
        return -1; /* Queue is empty and done */
    }

    /* Remove item */
    *item = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    pthread_cond_signal(&queue->cond_not_full);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

void fcp_queue_mark_done(fcp_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->done = true;
    pthread_cond_broadcast(&queue->cond_not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

bool fcp_queue_is_empty(fcp_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);
    bool empty = (queue->count == 0 && queue->done);
    pthread_mutex_unlock(&queue->mutex);
    return empty;
}

void fcp_queue_cleanup(fcp_queue_t *queue) {
    for (int i = 0; i < queue->count; i++) {
        int idx = (queue->head + i) % queue->capacity;
        free(queue->items[idx].src);
        free(queue->items[idx].dst);
    }
    free(queue->items);

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_not_empty);
    pthread_cond_destroy(&queue->cond_not_full);
}