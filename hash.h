/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_HASH_H
#define FCP_HASH_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define FCP_HASH_BLOCK_SIZE (64 * 1024) /* 64KB hash blocks */

/* SHA256 hash result (64 hex characters + null terminator) */
#define FCP_HASH_HEX_LEN 65

/* Compute SHA256 hash of a file. Returns allocated hex string (caller must free) */
char *fcp_hash_file(const char *path);

/* Compute SHA256 hash of a file into a provided buffer. Returns 0 on success. */
int fcp_hash_file_into(const char *path, char *hex_out, size_t hex_size);

/* Compare two SHA256 hex strings */
bool fcp_hash_compare(const char *hash1, const char *hash2);

#endif /* FCP_HASH_H */