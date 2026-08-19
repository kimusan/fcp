/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "identical.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

int fcp_check_identical(const char *src, const char *dst, bool verify_hash) {
    struct stat src_stat, dst_stat;

    /* Stat both files */
    if (stat(src, &src_stat) != 0) {
        return FCP_IDENTICAL_UNKNOWN;
    }
    if (stat(dst, &dst_stat) != 0) {
        return FCP_IDENTICAL_UNKNOWN; /* Destination doesn't exist */
    }

    /* Check 1: Same inode+device (hardlinked or same file) */
    if (src_stat.st_ino == dst_stat.st_ino && src_stat.st_dev == dst_stat.st_dev) {
        return FCP_IDENTICAL_YES;
    }

    /* Check 2: Different sizes -> definitely different */
    if (src_stat.st_size != dst_stat.st_size) {
        return FCP_IDENTICAL_NO;
    }

    /* Check 3: Same size and mtime -> likely identical */
    if (src_stat.st_mtime == dst_stat.st_mtime) {
        /* If verify_hash is requested, do full SHA256 comparison */
        if (verify_hash) {
            /* Hash comparison will be done by the caller using hash.c */
            return FCP_IDENTICAL_UNKNOWN;
        }
        return FCP_IDENTICAL_YES;
    }

    /* Different mtime, same size -> need hash comparison if enabled */
    if (verify_hash) {
        return FCP_IDENTICAL_UNKNOWN;
    }

    /* Default: copy (conservative) */
    return FCP_IDENTICAL_NO;
}