/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "colors.h"
#include "util.h"

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

/* Check if colors should be used (based on terminal and environment) */
bool fcp_use_colors(void) {
    /* Check NO_COLOR environment variable */
    if (getenv("NO_COLOR") != NULL) {
        return false;
    }

    /* Check if stderr is a terminal */
    return isatty_fd(STDERR_FILENO) != 0;
}

const char *fcp_color_progress_fill(void) {
    return fcp_use_colors() ? "\033[32m" : ""; /* Green */
}

const char *fcp_color_progress_empty(void) {
    return fcp_use_colors() ? "\033[90m" : ""; /* Gray */
}

const char *fcp_color_speed(void) {
    return fcp_use_colors() ? "\033[33m" : ""; /* Yellow */
}

const char *fcp_color_eta(void) {
    return fcp_use_colors() ? "\033[36m" : ""; /* Cyan */
}

const char *fcp_color_bold(void) {
    return fcp_use_colors() ? "\033[1m" : "";
}

const char *fcp_color_reset(void) {
    return fcp_use_colors() ? "\033[0m" : "";
}