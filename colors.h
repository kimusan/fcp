/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_COLORS_H
#define FCP_COLORS_H

#include <stdbool.h>

/* Enable or disable colors requested through configuration or the CLI. */
void fcp_set_colors_enabled(bool enabled);

/* Check if colors should be used (based on settings, terminal, and environment) */
bool fcp_use_colors(void);

/* Get color for progress bar fill */
const char *fcp_color_progress_fill(void);

/* Get color for progress bar empty */
const char *fcp_color_progress_empty(void);

/* Get color for speed */
const char *fcp_color_speed(void);

/* Get color for ETA */
const char *fcp_color_eta(void);

/* Get bold text prefix */
const char *fcp_color_bold(void);

/* Reset color */
const char *fcp_color_reset(void);

#endif /* FCP_COLORS_H */
