/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_PROGRESS_H
#define FCP_PROGRESS_H

#include <stdint.h>
#include <stdbool.h>

/* Progress state - updated by worker threads, read by progress thread */
typedef struct fcp_progress_s {
    bool enabled;
    bool active;

    /* Current file being copied */
    const char *current_file;
    uint64_t current_done;
    uint64_t current_total;

    /* Overall stats */
    uint64_t total_done;
    uint64_t total_all;
    double start_time;
    double last_update_time;

    /* Performance stats */
    double speed; /* bytes per second */
    double eta;   /* seconds */
} fcp_progress_t;

/* Initialize progress system */
void fcp_progress_init(fcp_progress_t *progress, bool enabled);

/* Update current file being copied */
void fcp_progress_update_file(fcp_progress_t *progress, const char *file, uint64_t total);

/* Update bytes done for current file */
void fcp_progress_update_done(fcp_progress_t *progress, uint64_t bytes);

/* Mark current file as complete */
void fcp_progress_complete_file(fcp_progress_t *progress);

/* Render progress to stderr (call from progress thread at ~10Hz) */
void fcp_progress_render(fcp_progress_t *progress);

/* Final cleanup */
void fcp_progress_cleanup(fcp_progress_t *progress);

#endif /* FCP_PROGRESS_H */