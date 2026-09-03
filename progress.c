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
#include <limits.h>
#include <sys/ioctl.h>

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

static int progress_bar_width(void) {
    struct winsize size;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
        return PROGRESS_BAR_WIDTH;
    }

    int width = size.ws_col - 65;
    if (width < 10) width = 10;
    if (width > PROGRESS_BAR_WIDTH) width = PROGRESS_BAR_WIDTH;
    return width;
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

void fcp_progress_set_parallel(fcp_progress_t *progress, bool parallel) {
    pthread_mutex_lock(&progress->mutex);
    progress->parallel_copy = parallel;
    progress->active_files = 0;
    progress->current_file = NULL;
    progress->current_done = 0;
    progress->current_total = 0;
    pthread_mutex_unlock(&progress->mutex);
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
    progress->last_speed_time = 0;
    progress->last_speed_bytes = 0;
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
    progress->last_speed_time = 0;
    progress->last_speed_bytes = progress->total_done;

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
    if (progress->parallel_copy) {
        progress->active_files++;
    } else {
        progress->current_file = file;
        progress->current_done = 0;
        progress->current_total = total;
    }
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
    if (progress->parallel_copy) {
        if (progress->active_files > 0) {
            progress->active_files--;
        }
    } else if (progress->current_file) {
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

    /* The final total is not known while scanning, so show activity instead of a percentage. */
    if (progress->phase == FCP_PHASE_SCANNING) {
        int bar_width = progress_bar_width();
        char bar[PROGRESS_BAR_WIDTH + 1];
        for (int i = 0; i < bar_width; i++) {
            bar[i] = ' ';
        }
        bar[bar_width] = '\0';

        int sweep = 2 * (bar_width - 1);
        int marker = (int)(now * 8.0) % sweep;
        if (marker >= bar_width) {
            marker = sweep - marker;
        }
        bar[marker] = '>';

        char done_size[32];
        char discovered_size[32];
        if (progress->total_all > 0) {
            snprintf(done_size, sizeof(done_size), "%s", format_size(progress->total_done));
            snprintf(discovered_size, sizeof(discovered_size), "%s", format_size(progress->total_all));
        }

        /* Clear line and show scanning activity. */
        if (use_color()) {
            fprintf(stderr, "\r\033[K" COLOR_BOLD "  Scanning..." COLOR_RESET
                    " %d files" COLOR_GRAY", %d identical" COLOR_RESET,
                    progress->files_scanned, progress->files_skipped_identical);
            if (progress->total_all > 0) {
                fprintf(stderr, " " COLOR_GREEN "[%s]" COLOR_RESET, bar);
                fprintf(stderr, " " COLOR_CYAN "%s" COLOR_RESET, done_size);
                fprintf(stderr, COLOR_GRAY " copied, " COLOR_RESET);
                fprintf(stderr, COLOR_CYAN "%s" COLOR_RESET, discovered_size);
                fprintf(stderr, COLOR_GRAY " discovered" COLOR_RESET);
            }
        } else {
            fprintf(stderr, "\r  Scanning... %d files, %d identical",
                    progress->files_scanned, progress->files_skipped_identical);
            if (progress->total_all > 0) {
                fprintf(stderr, " [%s] %s copied, %s discovered", bar,
                        done_size, discovered_size);
            }
        }
        fflush(stderr);
        pthread_mutex_unlock(&progress->mutex);
        return;
    }

    /* Smooth recent transfer samples instead of averaging in scan time. */
    if (progress->last_speed_time == 0) {
        progress->last_speed_time = now;
        progress->last_speed_bytes = progress->total_done;
        progress->speed = 0;
    } else if (now - progress->last_speed_time >= 0.25) {
        uint64_t bytes = progress->total_done - progress->last_speed_bytes;
        double instantaneous = (double)bytes / (now - progress->last_speed_time);
        progress->speed = progress->speed > 0 ?
                          0.65 * progress->speed + 0.35 * instantaneous : instantaneous;
        progress->last_speed_time = now;
        progress->last_speed_bytes = progress->total_done;
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
    int bar_width = progress_bar_width();
    int filled = (int)(bar_width * pct / 100.0);
    char bar[PROGRESS_BAR_WIDTH + 1];
    for (int i = 0; i < bar_width; i++) {
        bar[i] = (i < filled) ? '=' : ' ';
    }
    bar[bar_width] = '\0';

    char done_size[32];
    char total_size[32];
    snprintf(done_size, sizeof(done_size), "%s", format_size(progress->total_done));
    snprintf(total_size, sizeof(total_size), "%s",
             format_size(progress->total_all > 0 ? progress->total_all : progress->current_total));

    const char *current_file = progress->current_file;
    char truncated_file[PATH_MAX];
    struct winsize terminal_size;
    if (current_file && ioctl(STDERR_FILENO, TIOCGWINSZ, &terminal_size) == 0 &&
        terminal_size.ws_col > 0) {
        int file_width = terminal_size.ws_col - bar_width - 80;
        if (file_width < 8) {
            current_file = NULL;
        } else {
            if (file_width >= (int)sizeof(truncated_file)) {
                file_width = (int)sizeof(truncated_file) - 1;
            }
            truncate_string(current_file, truncated_file, (size_t)file_width);
            current_file = truncated_file;
        }
    }

    /* Clear an earlier, longer colored line before redrawing it. */
    fprintf(stderr, use_color() ? "\r\033[K" : "\r");

    if (use_color()) {
        /* Show scanned/skipped counter */
        fprintf(stderr, COLOR_BOLD "  Scanned:" COLOR_RESET " %d" COLOR_GRAY", Skipped:" COLOR_RESET " %d" COLOR_GRAY" | " COLOR_RESET,
                progress->files_scanned, progress->files_skipped_identical);
        
        if (current_file) {
            /* Show current file being copied */
            fprintf(stderr, COLOR_BOLD "%s" COLOR_RESET, current_file);
            fprintf(stderr, " " COLOR_GREEN "[%s]" COLOR_RESET " %5.1f%%",
                    bar, pct);
            fprintf(stderr, " %s%s%s/%s%s%s",
                    COLOR_CYAN, done_size, COLOR_RESET,
                    COLOR_CYAN, total_size, COLOR_RESET);
            fprintf(stderr, " %s%s%s" COLOR_GRAY" | ETA %s%s%s",
                    COLOR_YELLOW, format_speed(progress->speed), COLOR_RESET,
                    COLOR_CYAN, eta_str, COLOR_RESET);
        } else {
            if (progress->parallel_copy && progress->active_files > 0) {
                fprintf(stderr, "%u active file%s | ", progress->active_files,
                        progress->active_files == 1 ? "" : "s");
            }
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
