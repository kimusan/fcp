/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_COPY_H
#define FCP_COPY_H

#include <stdint.h>
#include <stdbool.h>

/* Copy result codes */
#define FCP_COPY_OK       0   /* Success */
#define FCP_COPY_ERROR   -1   /* Error */
#define FCP_COPY_SKIP     1   /* Skipped (identical file or dry-run) */

/* Reflink modes */
#define FCP_REFLINK_OFF   0
#define FCP_REFLINK_AUTO  1
#define FCP_REFLINK_ALWAYS 2

/* Forward declaration */
typedef struct fcp_progress_s fcp_progress_t;

/* Copy a file from src to dst. Returns FCP_COPY_OK, FCP_COPY_ERROR, or FCP_COPY_SKIP */
int fcp_copy_file(const char *src, const char *dst, int reflink_mode, bool dry_run,
                  uint64_t speed_limit, fcp_progress_t *progress);

#endif /* FCP_COPY_H */