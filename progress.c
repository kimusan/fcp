/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "progress.h"
#include "util.h"

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
    static int cached = -1;
    if (cached == -1) {
        cached = isatty_fd(STDERR_FILENO);
    }
    return cached != 0;
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
}

void fcp_progress_banner(fcp_progress_t *progress, const char *src, const char *dst) {
    if (!progress->enabled) return;

    fprintf(stderr, COLOR_BOLD "fcp" COLOR_RESET ": copying" COLOR_CYAN " %s" COLOR_RESET
            " -> " COLOR_CYAN " %s" COLOR_RESET "\n", src, dst);
    fflush(stderr);
}

void fcp_progress_set_scanning(fcp_progress_t *progress, int files_to_scan) {
    progress->phase = FCP_PHASE_SCANNING;
    progress->active = true;
    progress->files_scanned = 0;
    progress->files_skipped_identical = 0;
    progress->files_to_copy = files_to_scan;
    progress->last_update_time = 0; /* Force first render */
}

void fcp_progress_update_scanning(fcp_progress_t *progress, int files_found) {
    progress->files_scanned = files_found;
}

void fcp_progress_scanning_done(fcp_progress_t *progress, int files_skipped, int files_to_copy) {
    progress->files_skipped_identical = files_skipped;
    progress->files_to_copy = files_to_copy;
    progress->phase = FCP_PHASE_COPYING;

    if (!progress->enabled) return;

    fprintf(stderr, "  %d files to copy" COLOR_GRAY", %d identical skipped" COLOR_RESET "\n",
            files_to_copy, files_skipped);
    fflush(stderr);
}

void fcp_progress_update_file(fcp_progress_t *progress, const char *file, uint64_t total) {
    progress->current_file = file;
    progress->current_done = 0;
    progress->current_total = total;
    progress->active = true;
}

void fcp_progress_update_done(fcp_progress_t *progress, uint64_t bytes) {
    progress->current_done += bytes;

    /* Update overall stats */
    if (progress->current_done >= progress->current_total && progress->current_total > 0) {
        /* File just completed */
        progress->total_done += progress->current_total;
        progress->current_done = 0;
        progress->current_total = 0;
        progress->current_file = NULL;
    }
}

void fcp_progress_complete_file(fcp_progress_t *progress) {
    if (progress->current_file) {
        progress->total_done += progress->current_done;
        progress->current_done = 0;
        progress->current_total = 0;
        progress->current_file = NULL;
    }
}

void fcp_progress_render(fcp_progress_t *progress) {
    if (!progress->enabled || !progress->active) {
        return;
    }

    /* Throttle rendering */
    double now = clock_gettime_sec();
    double render_interval = (progress->phase == FCP_PHASE_SCANNING) ? 0.5 : 0.1;
    if (progress->last_update_time > 0 && now - progress->last_update_time < render_interval) {
        return;
    }
    progress->last_update_time = now;

    double elapsed = now - progress->start_time;

    /* Scanning phase: show file count */
    if (progress->phase == FCP_PHASE_SCANNING) {
        fprintf(stderr, "\r" COLOR_BOLD "  Scanning..." COLOR_RESET
                " %d files discovered", progress->files_scanned);
        if (progress->files_skipped_identical > 0) {
            fprintf(stderr, COLOR_GRAY " (%d already identical)" COLOR_RESET,
                    progress->files_skipped_identical);
        }
        fflush(stderr);
        return;
    }

    /* Calculate speed and ETA */
    if (elapsed > 0) {
        progress->speed = (double)progress->total_done / elapsed;
    }

    if (progress->total_all > 0 && progress->speed > 0) {
        uint64_t remaining = progress->total_all - progress->total_done;
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

    /* Clear line and render */
    fprintf(stderr, "\r");

    if (use_color()) {
        if (progress->current_file) {
            /* Show current file being copied */
            fprintf(stderr, COLOR_BOLD "%s" COLOR_RESET, progress->current_file);
            fprintf(stderr, " " COLOR_GREEN "[%s]" COLOR_RESET " %5.1f%%",
                    bar, pct);
            fprintf(stderr, " %s%s%s/%s%s%s",
                    COLOR_CYAN, format_size(progress->total_done), COLOR_RESET,
                    COLOR_CYAN, format_size(progress->total_all > 0 ? progress->total_all : progress->current_total), COLOR_RESET);
            fprintf(stderr, " %s%s%s" COLOR_GRAY" | ETA %s%s%s",
                    COLOR_YELLOW, format_speed(progress->speed), COLOR_RESET,
                    COLOR_CYAN, eta_str, COLOR_RESET);
        } else {
            fprintf(stderr, COLOR_GREEN "[%s]" COLOR_RESET " %5.1f%%",
                    bar, pct);
            fprintf(stderr, " %s%s%s/%s%s%s",
                    COLOR_CYAN, format_size(progress->total_done), COLOR_RESET,
                    COLOR_CYAN, format_size(progress->total_all), COLOR_RESET);
            fprintf(stderr, " %s%s%s" COLOR_GRAY" | ETA %s%s%s",
                    COLOR_YELLOW, format_speed(progress->speed), COLOR_RESET,
                    COLOR_CYAN, eta_str, COLOR_RESET);
        }
    } else {
        fprintf(stderr, "[%s] %5.1f%%", bar, pct);
        fprintf(stderr, " %s/%s", format_size(progress->total_done),
                format_size(progress->total_all > 0 ? progress->total_all : progress->current_total));
        fprintf(stderr, " %s", format_speed(progress->speed));
        fprintf(stderr, " ETA %s", eta_str);
    }

    fflush(stderr);
}

void fcp_progress_summary(fcp_progress_t *progress) {
    if (!progress->enabled) return;

    double elapsed = clock_gettime_sec() - progress->start_time;

    fprintf(stderr, "\r" COLOR_BOLD "fcp" COLOR_RESET ": done" COLOR_GRAY
            " in %.1fs" COLOR_RESET ", %s transferred",
            elapsed, format_size(progress->total_done));

    if (progress->files_skipped_identical > 0) {
        fprintf(stderr, COLOR_GRAY ", %d files skipped (identical)" COLOR_RESET,
                progress->files_skipped_identical);
    }

    fprintf(stderr, "\n");
    fflush(stderr);
}

void fcp_progress_cleanup(fcp_progress_t *progress) {
    if (progress->enabled) {
        /* Move to a new line */
        fprintf(stderr, "\r\033[K\n");
        fflush(stderr);
    }
}