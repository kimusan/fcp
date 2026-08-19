/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_IDENTICAL_H
#define FCP_IDENTICAL_H

#include <sys/types.h>
#include <stdbool.h>

/* Comparison result */
#define FCP_IDENTICAL_UNKNOWN  0
#define FCP_IDENTICAL_YES      1
#define FCP_IDENTICAL_NO       2

/* Check if two files are identical using the decision tree:
 *   1. Same inode+device -> identical
 *   2. Different size -> not identical
 *   3. Same size+mtime -> identical (unless verify_hash)
 *   4. Same size, different mtime, verify_hash -> SHA256 compare
 *
 * Returns FCP_IDENTICAL_YES, FCP_IDENTICAL_NO, or FCP_IDENTICAL_UNKNOWN
 */
int fcp_check_identical(const char *src, const char *dst, bool verify_hash);

#endif /* FCP_IDENTICAL_H */