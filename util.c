/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <errno.h>
#include <limits.h>

/* Internal buffers for format functions (thread-safe with thread-local storage) */
static __thread char size_buf[32];
static __thread char speed_buf[32];

const char *format_size(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%.1fT",
                 (double)bytes / (1024.0 * 1024 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%.1fG",
                 (double)bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%.1fM",
                 (double)bytes / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        snprintf(size_buf, sizeof(size_buf), "%.1fK",
                 (double)bytes / 1024.0);
    } else {
        snprintf(size_buf, sizeof(size_buf), "%lluB", (unsigned long long)bytes);
    }
    return size_buf;
}

const char *format_speed(double bytes_per_sec) {
    const char *unit = "B/s";
    double val = bytes_per_sec;

    if (val >= 1024.0 * 1024 * 1024 * 1024) {
        val /= 1024.0 * 1024 * 1024 * 1024;
        unit = "GB/s";
    } else if (val >= 1024.0 * 1024 * 1024) {
        val /= 1024.0 * 1024 * 1024;
        unit = "MB/s";
    } else if (val >= 1024.0 * 1024) {
        val /= 1024.0 * 1024;
        unit = "KB/s";
    }

    if (val >= 100.0) {
        snprintf(speed_buf, sizeof(speed_buf), "%.0f%s", val, unit);
    } else if (val >= 10.0) {
        snprintf(speed_buf, sizeof(speed_buf), "%.1f%s", val, unit);
    } else {
        snprintf(speed_buf, sizeof(speed_buf), "%.2f%s", val, unit);
    }
    return speed_buf;
}

int parse_size(const char *str, uint64_t *result) {
    char *endptr;
    double val = strtod(str, &endptr);

    if (endptr == str) {
        return -1; /* No digits found */
    }

    uint64_t multiplier = 1;
    if (*endptr == 'K' || *endptr == 'k') {
        multiplier = 1024;
        endptr++;
    } else if (*endptr == 'M' || *endptr == 'm') {
        multiplier = 1024 * 1024;
        endptr++;
    } else if (*endptr == 'G' || *endptr == 'g') {
        multiplier = 1024 * 1024 * 1024;
        endptr++;
    } else if (*endptr == 'T' || *endptr == 't') {
        multiplier = 1024ULL * 1024 * 1024 * 1024;
        endptr++;
    }

    if (*endptr != '\0') {
        return -1; /* Trailing garbage */
    }

    *result = (uint64_t)(val * multiplier);
    return 0;
}

int parse_parallel_count(const char *str, int *result) {
    if (strcmp(str, "auto") == 0) {
        *result = 0;
        return 0;
    }

    char *endptr;
    errno = 0;
    long value = strtol(str, &endptr, 10);
    if (endptr == str || *endptr != '\0' || errno == ERANGE ||
        value < 0 || value > INT_MAX) {
        return -1;
    }

    *result = (int)value;
    return 0;
}

int isatty_fd(int fd) {
    return isatty(fd);
}

int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int path_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

int path_is_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
}

int get_num_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) {
        n = 1;
    }
    return (int)n;
}

void truncate_string(const char *input, char *output, size_t max_len) {
    size_t input_len = strlen(input);
    if (input_len <= max_len) {
        strcpy(output, input);
        return;
    }

    size_t truncate_at = max_len - 3; /* Space for "..." */
    if (truncate_at < 3) {
        truncate_at = 3;
    }

    strncpy(output, input, truncate_at);
    output[truncate_at] = '\0';
    strcat(output, "...");
}
