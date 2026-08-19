/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#define _GNU_SOURCE

#include "copy.h"
#include "util.h"
#include "progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>

#ifndef FICLONE
#define FICLONE _IO(0x94, 9)
#endif

#define FCP_BUFFER_SIZE (1024 * 1024) /* 1MB copy buffer */

/* Clock helper for speed limiting */
static double clock_gettime_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int fcp_supports_reflink(const char *path) {
    /* Create a temp file in the same directory and test FICLONE */
    char *dir = strdup(path);
    if (!dir) return 0;

    char *slash = strrchr(dir, '/');
    if (slash) {
        *(slash + 1) = '\0';
    } else {
        strcpy(dir, "./");
    }

    char src_template[1024];
    snprintf(src_template, sizeof(src_template), "%sfcp_reflink_test_XXXXXX", dir);

    int fd_src = mkstemp(src_template);
    if (fd_src < 0) {
        free(dir);
        return 0;
    }

    /* Write some data */
    const char *test_data = "fcp reflink test";
    ssize_t dummy;
    dummy = write(fd_src, test_data, strlen(test_data));
    (void)dummy;

    char dst_template[1024];
    snprintf(dst_template, sizeof(dst_template), "%sfcp_reflink_dest_XXXXXX", dir);

    int fd_dst = mkstemp(dst_template);
    if (fd_dst < 0) {
        close(fd_src);
        unlink(src_template);
        free(dir);
        return 0;
    }

    /* Try FICLONE */
    int ret = ioctl(fd_dst, FICLONE, fd_src);

    close(fd_src);
    close(fd_dst);
    unlink(src_template);
    unlink(dst_template);
    free(dir);

    return (ret == 0);
}

int fcp_copy_file(const char *src, const char *dst, int reflink_mode, bool dry_run,
                  uint64_t speed_limit, fcp_progress_t *progress) {
    if (dry_run) {
        /* Just check if source exists */
        struct stat st;
        if (stat(src, &st) != 0) {
            fprintf(stderr, "fcp: cannot stat '%s': %s\n", src, strerror(errno));
            return FCP_COPY_ERROR;
        }
        return FCP_COPY_SKIP;
    }

    struct stat src_stat;
    if (stat(src, &src_stat) != 0) {
        fprintf(stderr, "fcp: cannot stat '%s': %s\n", src, strerror(errno));
        return FCP_COPY_ERROR;
    }

    /* If destination exists and is the same inode, skip */
    struct stat dst_stat;
    if (stat(dst, &dst_stat) == 0) {
        if (dst_stat.st_ino == src_stat.st_ino && dst_stat.st_dev == src_stat.st_dev) {
            return FCP_COPY_SKIP;
        }
    }

    /* Handle reflink if requested */
    if (reflink_mode != FCP_REFLINK_OFF) {
        int use_reflink = (reflink_mode == FCP_REFLINK_ALWAYS);

        if (!use_reflink) {
            /* AUTO mode: check if destination directory supports reflink */
            /* Get parent directory of destination */
            char *dst_copy = strdup(dst);
            if (dst_copy) {
                char *slash = strrchr(dst_copy, '/');
                if (slash) {
                    *slash = '\0';
                    use_reflink = fcp_supports_reflink(dst_copy);
                }
                free(dst_copy);
            }
        }

        if (use_reflink) {
            /* Try FICLONE */
            int fd_src = open(src, O_RDONLY);
            if (fd_src < 0) {
                fprintf(stderr, "fcp: cannot open '%s': %s\n", src, strerror(errno));
                return FCP_COPY_ERROR;
            }

            int fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode);
            if (fd_dst < 0) {
                close(fd_src);
                fprintf(stderr, "fcp: cannot create '%s': %s\n", dst, strerror(errno));
                return FCP_COPY_ERROR;
            }

            if (ioctl(fd_dst, FICLONE, fd_src) == 0) {
                close(fd_src);
                close(fd_dst);
                return FCP_COPY_OK;
            }

            /* FICLONE failed, fall through to regular copy */
            close(fd_src);
            unlink(dst);
        }
    }

    /* Regular copy with copy_file_range or read/write fallback */
    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) {
        fprintf(stderr, "fcp: cannot open '%s': %s\n", src, strerror(errno));
        return FCP_COPY_ERROR;
    }

    int fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode);
    if (fd_dst < 0) {
        close(fd_src);
        fprintf(stderr, "fcp: cannot create '%s': %s\n", dst, strerror(errno));
        return FCP_COPY_ERROR;
    }

    /* Advise the kernel for sequential access */
    posix_fadvise(fd_src, 0, 0, POSIX_FADV_SEQUENTIAL);

    uint64_t total_copied = 0;
    uint64_t file_size = src_stat.st_size;
    ssize_t bytes_read;
    char *buffer = malloc(FCP_BUFFER_SIZE);
    if (!buffer) {
        close(fd_src);
        close(fd_dst);
        fprintf(stderr, "fcp: out of memory\n");
        return FCP_COPY_ERROR;
    }

    double speed_start = clock_gettime_sec();
    double speed_last_update = speed_start;
    uint64_t speed_bytes = 0;

    while (total_copied < file_size) {
        /* Try copy_file_range first (zero-copy when possible) */
        ssize_t copied = copy_file_range(fd_src, NULL, fd_dst, NULL, file_size - total_copied, 0);

        ssize_t chunk_size = 0;
        if (copied > 0) {
            total_copied += copied;
            speed_bytes += copied;
            chunk_size = copied;
        } else {
            /* Fall back to read/write */
            bytes_read = read(fd_src, buffer, FCP_BUFFER_SIZE);
            if (bytes_read < 0) {
                if (errno == EINTR) continue;
                close(fd_src);
                close(fd_dst);
                free(buffer);
                fprintf(stderr, "fcp: read error on '%s': %s\n", src, strerror(errno));
                return FCP_COPY_ERROR;
            }

            if (bytes_read == 0) break; /* EOF */

            ssize_t bytes_written = 0;
            while (bytes_written < bytes_read) {
                ssize_t w = write(fd_dst, buffer + bytes_written, bytes_read - bytes_written);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    close(fd_src);
                    close(fd_dst);
                    free(buffer);
                    fprintf(stderr, "fcp: write error on '%s': %s\n", dst, strerror(errno));
                    return FCP_COPY_ERROR;
                }
                bytes_written += w;
            }

            total_copied += bytes_written;
            speed_bytes += bytes_written;
            chunk_size = bytes_written;
        }

        /* Update progress */
        if (progress && progress->enabled) {
            fcp_progress_update_done(progress, chunk_size);
            fcp_progress_render(progress);
        }

        /* Speed limiting */
        if (speed_limit > 0) {
            double now = clock_gettime_sec();
            double elapsed = now - speed_start;
            uint64_t expected_bytes = (uint64_t)(elapsed * speed_limit);

            if (total_copied > expected_bytes) {
                /* We're too fast, sleep to throttle */
                double needed_time = (double)total_copied / speed_limit;
                double sleep_time = needed_time - (now - speed_start);
                if (sleep_time > 0) {
                    struct timespec ts;
                    ts.tv_sec = (time_t)sleep_time;
                    ts.tv_nsec = (long)((sleep_time - ts.tv_sec) * 1e9);
                    nanosleep(&ts, NULL);
                }
            }
        }

        /* Update speed limit tracking timer */
        double now = clock_gettime_sec();
        if (now - speed_last_update >= 1.0) {
            speed_last_update = now;
            speed_bytes = 0;
        }
    }

    /* Preserve file metadata */
    struct timespec times[2];
    times[0].tv_sec = src_stat.st_atim.tv_sec;
    times[0].tv_nsec = src_stat.st_atim.tv_nsec;
    times[1].tv_sec = src_stat.st_mtim.tv_sec;
    times[1].tv_nsec = src_stat.st_mtim.tv_nsec;
    utimensat(AT_FDCWD, dst, times, 0);

    close(fd_src);
    close(fd_dst);
    free(buffer);

    return FCP_COPY_OK;
}