/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "identical.h"
#include "hash.h"

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

    /* If verify_hash is NOT enabled: use fast mtime check */
    if (!verify_hash) {
        if (src_stat.st_mtime == dst_stat.st_mtime) {
            return FCP_IDENTICAL_YES;
        }
        return FCP_IDENTICAL_NO;
    }

    /* Check 3: verify_hash is enabled -> compute and compare SHA256 */
    char src_hash[FCP_HASH_HEX_LEN];
    char dst_hash[FCP_HASH_HEX_LEN];

    if (fcp_hash_file_into(src, src_hash, sizeof(src_hash)) != 0 ||
        fcp_hash_file_into(dst, dst_hash, sizeof(dst_hash)) != 0) {
        return FCP_IDENTICAL_UNKNOWN;
    }

    if (strcmp(src_hash, dst_hash) == 0) {
        return FCP_IDENTICAL_YES;
    }

    return FCP_IDENTICAL_NO;
}