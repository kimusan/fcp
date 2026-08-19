/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_PROGRESS_H
#define FCP_PROGRESS_H

#include <stdint.h>
#include <stdbool.h>

/* Scanning/copy phase */
typedef enum {
    FCP_PHASE_IDLE = 0,
    FCP_PHASE_SCANNING,
    FCP_PHASE_COPYING,
    FCP_PHASE_DONE
} fcp_phase_t;

/* Progress state - updated by worker threads, read by progress thread */
typedef struct fcp_progress_s {
    bool enabled;
    bool active;

    /* Current phase */
    fcp_phase_t phase;

    /* Scanning info */
    int files_scanned;
    int files_skipped_identical;
    int files_to_copy;

    /* Current file being copied */
    const char *current_file;
    uint64_t current_done;
    uint64_t current_total;

    /* Overall stats */
    uint64_t total_done;
    uint64_t total_all;
    uint64_t total_bytes;  /* Total bytes found during scanning (all files, including identical) */
    double start_time;
    double last_update_time;

    /* Performance stats */
    double speed; /* bytes per second */
    double eta;   /* seconds */
} fcp_progress_t;

/* Initialize progress system */
void fcp_progress_init(fcp_progress_t *progress, bool enabled);

/* Set scanning phase with file count info */
void fcp_progress_set_scanning(fcp_progress_t *progress, int files_to_scan);

/* Update scanning progress (files found so far, bytes processed/skipped) */
void fcp_progress_update_scanning(fcp_progress_t *progress, int files_found, uint64_t bytes_processed);

/* Mark scanning complete with summary */
void fcp_progress_scanning_done(fcp_progress_t *progress, int files_skipped, int files_to_copy);

/* Update current file being copied */
void fcp_progress_update_file(fcp_progress_t *progress, const char *file, uint64_t total);

/* Update bytes done for current file */
void fcp_progress_update_done(fcp_progress_t *progress, uint64_t bytes);

/* Mark current file as complete */
void fcp_progress_complete_file(fcp_progress_t *progress);

/* Print startup banner */
void fcp_progress_banner(fcp_progress_t *progress, const char *src, const char *dst);

/* Print final summary */
void fcp_progress_summary(fcp_progress_t *progress);

/* Render progress to stderr (call from progress thread at ~10Hz) */
void fcp_progress_render(fcp_progress_t *progress);

/* Final cleanup */
void fcp_progress_cleanup(fcp_progress_t *progress);

#endif /* FCP_PROGRESS_H */