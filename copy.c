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

    /* If destination exists and is the same inode on the same device, skip */
    struct stat dst_stat;
    if (stat(dst, &dst_stat) == 0) {
        if (dst_stat.st_ino == src_stat.st_ino && dst_stat.st_dev == src_stat.st_dev) {
            return FCP_COPY_SKIP;
        }
    }

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

    uint64_t file_size = src_stat.st_size;

    /* Handle reflink directly on open destination descriptor */
    if (reflink_mode != FCP_REFLINK_OFF && file_size > 0) {
        if (ioctl(fd_dst, FICLONE, fd_src) == 0) {
            /* Instant reflink success */
            if (progress && progress->enabled) {
                fcp_progress_update_done(progress, file_size);
            }
            fchmod(fd_dst, src_stat.st_mode);
            struct timespec times[2];
            times[0] = src_stat.st_atim;
            times[1] = src_stat.st_mtim;
            futimens(fd_dst, times);
            close(fd_src);
            close(fd_dst);
            return FCP_COPY_OK;
        }

        if (reflink_mode == FCP_REFLINK_ALWAYS) {
            fprintf(stderr, "fcp: failed to clone '%s' to '%s': %s\n", src, dst, strerror(errno));
            close(fd_src);
            close(fd_dst);
            unlink(dst);
            return FCP_COPY_ERROR;
        }

        /* AUTO mode: reflink unsupported, reset destination and continue */
        if (ftruncate(fd_dst, 0) != 0) {
            /* ignore */
        }
        lseek(fd_src, 0, SEEK_SET);
        lseek(fd_dst, 0, SEEK_SET);
    }

    /* If 0-byte file, finish immediately */
    if (file_size == 0) {
        fchmod(fd_dst, src_stat.st_mode);
        struct timespec times[2];
        times[0] = src_stat.st_atim;
        times[1] = src_stat.st_mtim;
        futimens(fd_dst, times);
        close(fd_src);
        close(fd_dst);
        return FCP_COPY_OK;
    }

    /* Advise the kernel for sequential access */
    posix_fadvise(fd_src, 0, 0, POSIX_FADV_SEQUENTIAL);

    uint64_t total_copied = 0;
    char *buffer = malloc(FCP_BUFFER_SIZE);
    if (!buffer) {
        close(fd_src);
        close(fd_dst);
        fprintf(stderr, "fcp: out of memory\n");
        return FCP_COPY_ERROR;
    }

    double speed_start = clock_gettime_sec();
    double speed_last_update = speed_start;
    bool try_cfr = true;

    while (total_copied < file_size) {
        ssize_t chunk_size = 0;

        if (try_cfr) {
            ssize_t copied = copy_file_range(fd_src, NULL, fd_dst, NULL, file_size - total_copied, 0);
            if (copied > 0) {
                total_copied += copied;
                chunk_size = copied;
            } else if (copied < 0) {
                if (errno == EINTR) {
                    continue;
                }
                /* Disable copy_file_range on failure (e.g. EXDEV, EINVAL, ENOSYS) */
                try_cfr = false;
            } else {
                /* copied == 0 (EOF reached earlier than expected) */
                break;
            }
        }

        if (!try_cfr) {
            /* Fall back to read/write */
            size_t to_read = FCP_BUFFER_SIZE;
            if (file_size - total_copied < to_read) {
                to_read = file_size - total_copied;
            }

            ssize_t bytes_read = read(fd_src, buffer, to_read);
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
            chunk_size = bytes_written;
        }

        /* Update progress */
        if (progress && progress->enabled) {
            fcp_progress_update_done(progress, chunk_size);
        }

        /* Speed limiting */
        if (speed_limit > 0) {
            double now = clock_gettime_sec();
            double elapsed = now - speed_start;
            uint64_t expected_bytes = (uint64_t)(elapsed * speed_limit);

            if (total_copied > expected_bytes) {
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

        double now = clock_gettime_sec();
        if (now - speed_last_update >= 1.0) {
            speed_last_update = now;
        }
    }

    /* Preserve file permissions and timestamps */
    fchmod(fd_dst, src_stat.st_mode);
    struct timespec times[2];
    times[0] = src_stat.st_atim;
    times[1] = src_stat.st_mtim;
    futimens(fd_dst, times);

    close(fd_src);
    close(fd_dst);
    free(buffer);

    return FCP_COPY_OK;
}