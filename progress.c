/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "progress.h"
#include "util.h"
#include "colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#define PROGRESS_BAR_WIDTH 40
#define PROGRESS_UPDATE_INTERVAL 0.1 /* 100ms */

/* Color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_GRAY    "\033[90m"

static bool use_color(void) {
    return fcp_use_colors();
}

static double clock_gettime_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void fcp_progress_init(fcp_progress_t *progress, bool enabled) {
    memset(progress, 0, sizeof(*progress));
    progress->enabled = enabled;
    progress->active = false;
    progress->phase = FCP_PHASE_IDLE;
    progress->start_time = clock_gettime_sec();
    progress->last_update_time = progress->start_time;
    pthread_mutex_init(&progress->mutex, NULL);
}

void fcp_progress_banner(fcp_progress_t *progress, const char *src, const char *dst) {
    if (!progress->enabled) return;

    pthread_mutex_lock(&progress->mutex);
    if (use_color()) {
        fprintf(stderr, COLOR_BOLD "fcp" COLOR_RESET ": copying" COLOR_CYAN " %s" COLOR_RESET
                " -> " COLOR_CYAN " %s" COLOR_RESET "\n", src, dst);
    } else {
        fprintf(stderr, "fcp: copying %s -> %s\n", src, dst);
    }
    fflush(stderr);
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_set_scanning(fcp_progress_t *progress, int files_to_scan) {
    pthread_mutex_lock(&progress->mutex);
    progress->phase = FCP_PHASE_SCANNING;
    progress->active = true;
    progress->files_scanned = 0;
    progress->files_skipped_identical = 0;
    progress->files_to_copy = files_to_scan;
    progress->total_bytes = 0;
    progress->total_done = 0;
    progress->total_all = 0;
    progress->last_update_time = 0; /* Force first render */
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_update_scanning(fcp_progress_t *progress, int files_found,
                                  int files_skipped, uint64_t total_bytes_found) {
    pthread_mutex_lock(&progress->mutex);
    progress->files_scanned = files_found;
    progress->files_skipped_identical = files_skipped;
    /* Track total bytes found so we can show progress during scanning */
    if (total_bytes_found > progress->total_all) {
        progress->total_all = total_bytes_found;
    }
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_scanning_done(fcp_progress_t *progress, int files_skipped, int files_to_copy) {
    pthread_mutex_lock(&progress->mutex);
    progress->files_skipped_identical = files_skipped;
    progress->files_to_copy = files_to_copy;
    progress->phase = FCP_PHASE_COPYING;
    progress->last_update_time = 0; /* Force first render in copy phase */

    if (progress->enabled) {
        /* Clear scanning progress line and move to new line */
        fprintf(stderr, use_color() ? "\r\033[K" : "\r");
        fprintf(stderr, "\n");
        if (use_color()) {
            fprintf(stderr, "  %d files to copy" COLOR_GRAY", %d identical skipped" COLOR_RESET "\n",
                    files_to_copy, files_skipped);
        } else {
            fprintf(stderr, "  %d files to copy, %d identical skipped\n",
                    files_to_copy, files_skipped);
        }
        fflush(stderr);
    }
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_set_total_bytes(fcp_progress_t *progress, uint64_t total_bytes) {
    pthread_mutex_lock(&progress->mutex);
    progress->total_all = total_bytes;
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_add_skipped_bytes(fcp_progress_t *progress, uint64_t bytes) {
    pthread_mutex_lock(&progress->mutex);
    progress->total_done += bytes;
    pthread_mutex_unlock(&progress->mutex);
}

/* Update progress counters during copy phase (called from copy functions) */
void fcp_progress_update_copy(fcp_progress_t *progress) {
    pthread_mutex_lock(&progress->mutex);
    /* Copy atomic counters to progress struct for display */
    progress->files_scanned = atomic_load(&g_files_scanned);
    progress->files_skipped_identical = atomic_load(&g_files_skipped);
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_update_file(fcp_progress_t *progress, const char *file, uint64_t total) {
    pthread_mutex_lock(&progress->mutex);
    progress->current_file = file;
    progress->current_done = 0;
    progress->current_total = total;
    progress->active = true;
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_update_done(fcp_progress_t *progress, uint64_t bytes) {
    pthread_mutex_lock(&progress->mutex);
    progress->current_done += bytes;
    progress->total_done += bytes;
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_complete_file(fcp_progress_t *progress) {
    pthread_mutex_lock(&progress->mutex);
    if (progress->current_file) {
        progress->current_done = 0;
        progress->current_total = 0;
        progress->current_file = NULL;
    }
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_render(fcp_progress_t *progress) {
    if (!progress->enabled || !progress->active) {
        return;
    }

    pthread_mutex_lock(&progress->mutex);

    /* Throttle rendering */
    double now = clock_gettime_sec();
    double render_interval = (progress->phase == FCP_PHASE_SCANNING) ? 0.5 : 0.1;
    if (progress->last_update_time > 0 && now - progress->last_update_time < render_interval) {
        pthread_mutex_unlock(&progress->mutex);
        return;
    }
    progress->last_update_time = now;

    double elapsed = now - progress->start_time;

    /* Scanning phase: show progress bar with skipped files counting as progress */
    if (progress->phase == FCP_PHASE_SCANNING) {
        double pct = 0;
        if (progress->total_all > 0) {
            pct = 100.0 * (double)progress->total_done / (double)progress->total_all;
        }

        int filled = (int)(PROGRESS_BAR_WIDTH * pct / 100.0);
        char bar[PROGRESS_BAR_WIDTH + 1];
        for (int i = 0; i < PROGRESS_BAR_WIDTH; i++) {
            bar[i] = (i < filled) ? '=' : ' ';
        }
        bar[PROGRESS_BAR_WIDTH] = '\0';

        /* Clear line and show scanning info with progress bar */
        if (use_color()) {
            fprintf(stderr, "\r\033[K" COLOR_BOLD "  Scanning..." COLOR_RESET
                    " %d files" COLOR_GRAY", %d identical" COLOR_RESET,
                    progress->files_scanned, progress->files_skipped_identical);
        } else {
            fprintf(stderr, "\r  Scanning... %d files, %d identical",
                    progress->files_scanned, progress->files_skipped_identical);
        }
        if (progress->total_all > 0) {
            fprintf(stderr, " " COLOR_GREEN "[%s]" COLOR_RESET " %5.1f%%",
                    bar, pct);
            fprintf(stderr, " " COLOR_CYAN "%s" COLOR_RESET, format_size(progress->total_done));
            fprintf(stderr, COLOR_GRAY "/" COLOR_RESET);
            fprintf(stderr, COLOR_CYAN "%s" COLOR_RESET, format_size(progress->total_all));
        }
        fflush(stderr);
        pthread_mutex_unlock(&progress->mutex);
        return;
    }

    /* Calculate speed and ETA */
    if (elapsed > 0) {
        progress->speed = (double)progress->total_done / elapsed;
    }

    if (progress->total_all > 0 && progress->speed > 0) {
        uint64_t remaining = (progress->total_all > progress->total_done) ? (progress->total_all - progress->total_done) : 0;
        progress->eta = (double)remaining / progress->speed;
    } else {
        progress->eta = 0;
    }

    /* Format ETA */
    char eta_str[64];
    if (progress->eta > 0 && progress->eta < 3600) {
        snprintf(eta_str, sizeof(eta_str), "%ldm %02lds",
                 (long)(progress->eta / 60), (long)fmod(progress->eta, 60));
    } else if (progress->eta >= 3600) {
        snprintf(eta_str, sizeof(eta_str), "%.1fh", progress->eta / 3600.0);
    } else {
        snprintf(eta_str, sizeof(eta_str), "%.1fs", progress->eta);
    }

    /* Calculate overall progress percentage */
    double pct = 0;
    if (progress->total_all > 0) {
        pct = 100.0 * (double)progress->total_done / (double)progress->total_all;
    }

    /* Build progress bar string */
    int filled = (int)(PROGRESS_BAR_WIDTH * pct / 100.0);
    char bar[PROGRESS_BAR_WIDTH + 1];
    for (int i = 0; i < PROGRESS_BAR_WIDTH; i++) {
        bar[i] = (i < filled) ? '=' : ' ';
    }
    bar[PROGRESS_BAR_WIDTH] = '\0';

    char done_size[32];
    char total_size[32];
    snprintf(done_size, sizeof(done_size), "%s", format_size(progress->total_done));
    snprintf(total_size, sizeof(total_size), "%s",
             format_size(progress->total_all > 0 ? progress->total_all : progress->current_total));

    /* Clear line and render */
    fprintf(stderr, "\r");

    if (use_color()) {
        /* Show scanned/skipped counter */
        fprintf(stderr, COLOR_BOLD "  Scanned:" COLOR_RESET " %d" COLOR_GRAY", Skipped:" COLOR_RESET " %d" COLOR_GRAY" | " COLOR_RESET,
                progress->files_scanned, progress->files_skipped_identical);
        
        if (progress->current_file) {
            /* Show current file being copied */
            fprintf(stderr, COLOR_BOLD "%s" COLOR_RESET, progress->current_file);
            fprintf(stderr, " " COLOR_GREEN "[%s]" COLOR_RESET " %5.1f%%",
                    bar, pct);
            fprintf(stderr, " %s%s%s/%s%s%s",
                    COLOR_CYAN, done_size, COLOR_RESET,
                    COLOR_CYAN, total_size, COLOR_RESET);
            fprintf(stderr, " %s%s%s" COLOR_GRAY" | ETA %s%s%s",
                    COLOR_YELLOW, format_speed(progress->speed), COLOR_RESET,
                    COLOR_CYAN, eta_str, COLOR_RESET);
        } else {
            fprintf(stderr, COLOR_GREEN "[%s]" COLOR_RESET " %5.1f%%",
                    bar, pct);
            fprintf(stderr, " %s%s%s/%s%s%s",
                    COLOR_CYAN, done_size, COLOR_RESET,
                    COLOR_CYAN, total_size, COLOR_RESET);
            fprintf(stderr, " %s%s%s" COLOR_GRAY" | ETA %s%s%s",
                    COLOR_YELLOW, format_speed(progress->speed), COLOR_RESET,
                    COLOR_CYAN, eta_str, COLOR_RESET);
        }
    } else {
        fprintf(stderr, "Scanned: %d, Skipped: %d | ",
                progress->files_scanned, progress->files_skipped_identical);
        fprintf(stderr, "[%s] %5.1f%%", bar, pct);
        fprintf(stderr, " %s/%s", done_size, total_size);
        fprintf(stderr, " %s", format_speed(progress->speed));
        fprintf(stderr, " ETA %s", eta_str);
    }

    fflush(stderr);
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_summary(fcp_progress_t *progress) {
    if (!progress->enabled) return;

    pthread_mutex_lock(&progress->mutex);
    /* Force one final redraw with the bytes actually accounted for. */
    progress->last_update_time = 0;
    pthread_mutex_unlock(&progress->mutex);

    fcp_progress_render(progress);
    fprintf(stderr, "\n");

    pthread_mutex_lock(&progress->mutex);
    double elapsed = clock_gettime_sec() - progress->start_time;

    if (use_color()) {
        fprintf(stderr, "\r" COLOR_BOLD "fcp" COLOR_RESET ": done" COLOR_GRAY
                " in %.1fs" COLOR_RESET ", %s transferred",
                elapsed, format_size(progress->total_done));
    } else {
        fprintf(stderr, "\rfcp: done in %.1fs, %s transferred",
                elapsed, format_size(progress->total_done));
    }

    if (progress->files_skipped_identical > 0) {
        if (use_color()) {
            fprintf(stderr, COLOR_GRAY ", %d files skipped (identical)" COLOR_RESET,
                    progress->files_skipped_identical);
        } else {
            fprintf(stderr, ", %d files skipped (identical)",
                    progress->files_skipped_identical);
        }
    }

    fprintf(stderr, "\n");
    fflush(stderr);
    pthread_mutex_unlock(&progress->mutex);
}

void fcp_progress_cleanup(fcp_progress_t *progress) {
    if (progress->enabled) {
        /* Move to a new line */
        fprintf(stderr, use_color() ? "\r\033[K\n" : "\r\n");
        fflush(stderr);
    }
    pthread_mutex_destroy(&progress->mutex);
}
