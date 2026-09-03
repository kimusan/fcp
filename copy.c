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
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
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

/* Create an exclusive staging file alongside dst for an atomic replacement. */
static int create_atomic_temp(const char *dst, char *tmp_dst, size_t tmp_dst_size) {
    const char *slash = strrchr(dst, '/');
    int length;

    if (!slash) {
        length = snprintf(tmp_dst, tmp_dst_size, ".fcp_tmp.XXXXXX");
    } else if (slash == dst) {
        length = snprintf(tmp_dst, tmp_dst_size, "/.fcp_tmp.XXXXXX");
    } else {
        length = snprintf(tmp_dst, tmp_dst_size, "%.*s/.fcp_tmp.XXXXXX",
                          (int)(slash - dst), dst);
    }

    if (length < 0 || (size_t)length >= tmp_dst_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return mkostemp(tmp_dst, O_CLOEXEC);
}

/* Copy extended attributes between file descriptors or paths */
int fcp_copy_xattrs(int fd_src, int fd_dst, const char *src_path, const char *dst_path) {
    ssize_t list_len;
    char list[65536];

    if (fd_src >= 0) {
        list_len = flistxattr(fd_src, list, sizeof(list));
    } else if (src_path) {
        list_len = llistxattr(src_path, list, sizeof(list));
    } else {
        return 0;
    }

    if (list_len <= 0) return 0;

    char val[65536];
    for (const char *name = list; name < list + list_len; name += strlen(name) + 1) {
        ssize_t val_len;
        if (fd_src >= 0) {
            val_len = fgetxattr(fd_src, name, val, sizeof(val));
        } else {
            val_len = lgetxattr(src_path, name, val, sizeof(val));
        }

        if (val_len < 0) continue;

        if (fd_dst >= 0) {
            fsetxattr(fd_dst, name, val, val_len, 0);
        } else if (dst_path) {
            lsetxattr(dst_path, name, val, val_len, 0);
        }
    }
    return 0;
}

/* Helper to apply metadata (permissions, ownership, timestamps, xattrs) */
static void apply_metadata(int fd_dst, const char *dst_path, const struct stat *src_stat,
                           int preserve_flags, int fd_src, const char *src_path) {
    if (preserve_flags & FCP_PRESERVE_MODE) {
        if (fd_dst >= 0) fchmod(fd_dst, src_stat->st_mode);
        else if (dst_path) chmod(dst_path, src_stat->st_mode);
    }

    if (preserve_flags & FCP_PRESERVE_OWNERSHIP) {
        if (fd_dst >= 0) {
            if (fchown(fd_dst, src_stat->st_uid, src_stat->st_gid) != 0 && errno != EPERM) {
                /* ignore EPERM when not running as root */
            }
        } else if (dst_path) {
            if (lchown(dst_path, src_stat->st_uid, src_stat->st_gid) != 0 && errno != EPERM) {
                /* ignore EPERM */
            }
        }
    }

    if (preserve_flags & FCP_PRESERVE_XATTR) {
        fcp_copy_xattrs(fd_src, fd_dst, src_path, dst_path);
    }

    if (preserve_flags & FCP_PRESERVE_TIMESTAMPS) {
        struct timespec times[2];
        times[0] = src_stat->st_atim;
        times[1] = src_stat->st_mtim;
        if (fd_dst >= 0) futimens(fd_dst, times);
        else if (dst_path) utimensat(AT_FDCWD, dst_path, times, AT_SYMLINK_NOFOLLOW);
    }
}

int fcp_copy_file(const char *src, const char *dst, int reflink_mode, int sparse_mode,
                  int preserve_flags, bool atomic_mode, bool dry_run,
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
    if (fstat(fd_src, &src_stat) != 0) {
        fprintf(stderr, "fcp: cannot stat '%s': %s\n", src, strerror(errno));
        close(fd_src);
        return FCP_COPY_ERROR;
    }

    char tmp_dst[PATH_MAX + 128];
    const char *target_dst = dst;

    int fd_dst;
    if (atomic_mode) {
        fd_dst = create_atomic_temp(dst, tmp_dst, sizeof(tmp_dst));
        target_dst = tmp_dst;
    } else {
        fd_dst = open(target_dst, O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode);
    }
    if (fd_dst < 0) {
        close(fd_src);
        fprintf(stderr, "fcp: cannot create '%s': %s\n", target_dst, strerror(errno));
        return FCP_COPY_ERROR;
    }

    uint64_t file_size = src_stat.st_size;

    /* Handle reflink directly on open destination descriptor */
    if (reflink_mode != FCP_REFLINK_OFF && file_size > 0) {
        if (ioctl(fd_dst, FICLONE, fd_src) == 0) {
            /* Instant reflink success */
            if (progress && progress->enabled) {
                fcp_progress_update_done(progress, file_size);
                fcp_progress_render(progress);
            }
            apply_metadata(fd_dst, target_dst, &src_stat, preserve_flags, fd_src, src);
            if (atomic_mode) {
                fdatasync(fd_dst);
                close(fd_dst);
                if (rename(tmp_dst, dst) != 0) {
                    unlink(tmp_dst);
                    close(fd_src);
                    return FCP_COPY_ERROR;
                }
            } else {
                close(fd_dst);
            }
            close(fd_src);
            return FCP_COPY_OK;
        }

        if (reflink_mode == FCP_REFLINK_ALWAYS) {
            fprintf(stderr, "fcp: failed to clone '%s' to '%s': %s\n", src, dst, strerror(errno));
            close(fd_src);
            close(fd_dst);
            unlink(target_dst);
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
        apply_metadata(fd_dst, target_dst, &src_stat, preserve_flags, fd_src, src);
        if (atomic_mode) {
            fdatasync(fd_dst);
            close(fd_dst);
            if (rename(tmp_dst, dst) != 0) {
                unlink(tmp_dst);
                close(fd_src);
                return FCP_COPY_ERROR;
            }
        } else {
            close(fd_dst);
        }
        close(fd_src);
        return FCP_COPY_OK;
    }

    /* Determine if sparse file copying should be attempted */
    bool is_sparse = false;
    if (sparse_mode == FCP_SPARSE_ALWAYS) {
        is_sparse = true;
    } else if (sparse_mode == FCP_SPARSE_AUTO) {
        /* Auto-detect sparse file: allocated 512-byte blocks < logical size */
        if ((uint64_t)src_stat.st_blocks * 512 < file_size) {
            is_sparse = true;
        }
    }

    char *buffer = malloc(FCP_BUFFER_SIZE);
    if (!buffer) {
        close(fd_src);
        close(fd_dst);
        if (atomic_mode) unlink(tmp_dst);
        fprintf(stderr, "fcp: out of memory\n");
        return FCP_COPY_ERROR;
    }

    bool sparse_copy_success = false;

    if (is_sparse) {
        /* Truncate destination to create full sparse length */
        if (ftruncate(fd_dst, file_size) == 0) {
            off_t offset = 0;
            bool seek_ok = true;
            uint64_t sparse_copied = 0;

            while (offset < (off_t)file_size) {
                off_t data_start = lseek(fd_src, offset, SEEK_DATA);
                if (data_start < 0) {
                    if (errno == ENXIO) {
                        /* No more data extents (the rest is a hole) */
                        break;
                    }
                    /* Filesystem doesn't support SEEK_DATA */
                    seek_ok = false;
                    break;
                }

                off_t data_end = lseek(fd_src, data_start, SEEK_HOLE);
                if (data_end < 0) {
                    if (errno == ENXIO) {
                        data_end = (off_t)file_size;
                    } else {
                        seek_ok = false;
                        break;
                    }
                }
                if (data_end > (off_t)file_size) {
                    data_end = (off_t)file_size;
                }

                if (lseek(fd_dst, data_start, SEEK_SET) < 0 ||
                    lseek(fd_src, data_start, SEEK_SET) < 0) {
                    seek_ok = false;
                    break;
                }

                uint64_t extent_bytes = data_end - data_start;
                while (extent_bytes > 0) {
                    size_t to_read = (extent_bytes > FCP_BUFFER_SIZE) ? FCP_BUFFER_SIZE : extent_bytes;
                    ssize_t r = read(fd_src, buffer, to_read);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        seek_ok = false;
                        break;
                    }
                    if (r == 0) {
                        seek_ok = false;
                        break;
                    }

                    ssize_t w_total = 0;
                    while (w_total < r) {
                        ssize_t w = write(fd_dst, buffer + w_total, r - w_total);
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            seek_ok = false;
                            break;
                        }
                        w_total += w;
                    }
                    if (!seek_ok) break;

                    extent_bytes -= w_total;
                    sparse_copied += w_total;
                    if (progress && progress->enabled) {
                        fcp_progress_update_done(progress, w_total);
                        fcp_progress_render(progress);
                    }
                }

                if (!seek_ok) break;
                offset = data_end;
            }

            if (seek_ok) {
                sparse_copy_success = true;
            } else {
                /* Rewind for dense copy fallback */
                if (ftruncate(fd_dst, 0) != 0) {
                    /* ignore */
                }
                lseek(fd_src, 0, SEEK_SET);
                lseek(fd_dst, 0, SEEK_SET);
            }
        }
    }

    if (!sparse_copy_success) {
        /* Advise sequential access */
        posix_fadvise(fd_src, 0, 0, POSIX_FADV_SEQUENTIAL);

        uint64_t total_copied = 0;
        double speed_start = clock_gettime_sec();
        double speed_last_update = speed_start;
        bool try_cfr = true;

        /* Update copy phase counters before rendering */
        if (progress && progress->enabled) {
            fcp_progress_update_copy(progress);
        }

        while (total_copied < file_size) {
            ssize_t chunk_size = 0;

            if (try_cfr) {
                ssize_t copied = copy_file_range(fd_src, NULL, fd_dst, NULL, file_size - total_copied, 0);
                if (copied > 0) {
                    total_copied += copied;
                    chunk_size = copied;
                } else if (copied < 0) {
                    if (errno == EINTR) continue;
                    /* Disable CFR on error (e.g., EXDEV, EINVAL, ENOSYS) */
                    try_cfr = false;
                } else {
                    close(fd_src);
                    close(fd_dst);
                    free(buffer);
                    if (atomic_mode) unlink(tmp_dst);
                    fprintf(stderr, "fcp: unexpected end of '%s'\n", src);
                    return FCP_COPY_ERROR;
                }
            }

            if (!try_cfr) {
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
                    if (atomic_mode) unlink(tmp_dst);
                    fprintf(stderr, "fcp: read error on '%s': %s\n", src, strerror(errno));
                    return FCP_COPY_ERROR;
                }

                if (bytes_read == 0) {
                    close(fd_src);
                    close(fd_dst);
                    free(buffer);
                    if (atomic_mode) unlink(tmp_dst);
                    fprintf(stderr, "fcp: unexpected end of '%s'\n", src);
                    return FCP_COPY_ERROR;
                }

                ssize_t bytes_written = 0;
                while (bytes_written < bytes_read) {
                    ssize_t w = write(fd_dst, buffer + bytes_written, bytes_read - bytes_written);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        close(fd_src);
                        close(fd_dst);
                        free(buffer);
                        if (atomic_mode) unlink(tmp_dst);
                        fprintf(stderr, "fcp: write error on '%s': %s\n", target_dst, strerror(errno));
                        return FCP_COPY_ERROR;
                    }
                    bytes_written += w;
                }

                total_copied += bytes_written;
                chunk_size = bytes_written;
            }

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
    }

    struct stat final_src_stat;
    if (fstat(fd_src, &final_src_stat) != 0 ||
        final_src_stat.st_size != src_stat.st_size ||
        final_src_stat.st_mtim.tv_sec != src_stat.st_mtim.tv_sec ||
        final_src_stat.st_mtim.tv_nsec != src_stat.st_mtim.tv_nsec) {
        close(fd_src);
        close(fd_dst);
        free(buffer);
        if (atomic_mode) unlink(tmp_dst);
        fprintf(stderr, "fcp: source changed during copy '%s'\n", src);
        return FCP_COPY_ERROR;
    }

    /* Apply permissions, ownership, timestamps, and xattrs */
    apply_metadata(fd_dst, target_dst, &src_stat, preserve_flags, fd_src, src);

    if (atomic_mode) {
        fdatasync(fd_dst);
        close(fd_dst);
        if (rename(tmp_dst, dst) != 0) {
            fprintf(stderr, "fcp: atomic rename failed '%s' -> '%s': %s\n", tmp_dst, dst, strerror(errno));
            unlink(tmp_dst);
            close(fd_src);
            free(buffer);
            return FCP_COPY_ERROR;
        }
    } else {
        close(fd_dst);
    }

    close(fd_src);
    free(buffer);

    return FCP_COPY_OK;
}
