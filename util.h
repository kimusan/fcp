/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_UTIL_H
#define FCP_UTIL_H

#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>

/* Format byte count into human-readable string (e.g., "1.2G", "456M", "10K") */
const char *format_size(uint64_t bytes);

/* Format bytes per second into speed string (e.g., "125MB/s", "1.2GB/s") */
const char *format_speed(double bytes_per_sec);

/* Parse size string like "10M", "1G", "500K" into bytes. Returns 0 on success. */
int parse_size(const char *str, uint64_t *result);

/* Check if file descriptor is a terminal */
int isatty_fd(int fd);

/* Check if a path exists */
int path_exists(const char *path);

/* Check if a path is a directory */
int path_is_dir(const char *path);

/* Check if a path is a regular file */
int path_is_file(const char *path);

/* Get number of CPU cores (for parallelism defaults) */
int get_num_cores(void);

/* Truncate string to max length, adding "..." if truncated */
void truncate_string(const char *input, char *output, size_t max_len);

#endif /* FCP_UTIL_H */