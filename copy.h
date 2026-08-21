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
#define FCP_REFLINK_OFF    0
#define FCP_REFLINK_AUTO   1
#define FCP_REFLINK_ALWAYS 2

/* Sparse copy modes */
#define FCP_SPARSE_OFF     0
#define FCP_SPARSE_AUTO    1
#define FCP_SPARSE_ALWAYS  2

/* Metadata preservation flags */
#define FCP_PRESERVE_NONE       0
#define FCP_PRESERVE_MODE       (1 << 0)
#define FCP_PRESERVE_OWNERSHIP  (1 << 1)
#define FCP_PRESERVE_TIMESTAMPS (1 << 2)
#define FCP_PRESERVE_XATTR      (1 << 3)
#define FCP_PRESERVE_DEFAULT    (FCP_PRESERVE_MODE | FCP_PRESERVE_TIMESTAMPS)
#define FCP_PRESERVE_ALL        (FCP_PRESERVE_MODE | FCP_PRESERVE_OWNERSHIP | FCP_PRESERVE_TIMESTAMPS | FCP_PRESERVE_XATTR)

/* Forward declaration */
typedef struct fcp_progress_s fcp_progress_t;

/* Copy a file from src to dst. Returns FCP_COPY_OK, FCP_COPY_ERROR, or FCP_COPY_SKIP */
int fcp_copy_file(const char *src, const char *dst, int reflink_mode, int sparse_mode,
                  int preserve_flags, bool atomic_mode, bool dry_run,
                  uint64_t speed_limit, fcp_progress_t *progress);

/* Copy extended attributes between file descriptors or paths */
int fcp_copy_xattrs(int fd_src, int fd_dst, const char *src_path, const char *dst_path);

#endif /* FCP_COPY_H */