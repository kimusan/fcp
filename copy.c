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

static pthread_mutex_t speed_limit_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t speed_limit_rate = 0;
static uint64_t speed_limit_bytes = 0;
static double speed_limit_start = 0;

/* Clock helper for speed limiting */
static double clock_gettime_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Reserve transfer time against a process-wide bandwidth limit. */
static void limit_copy_speed(uint64_t bytes, uint64_t rate) {
    if (rate == 0 || bytes == 0) {
        return;
    }

    pthread_mutex_lock(&speed_limit_mutex);
    if (speed_limit_rate != rate) {
        speed_limit_rate = rate;
        speed_limit_bytes = 0;
        speed_limit_start = clock_gettime_sec();
    }

    speed_limit_bytes += bytes;
    double deadline = speed_limit_start + (double)speed_limit_bytes / rate;
    double remaining = deadline - clock_gettime_sec();
    if (remaining > 0) {
        struct timespec sleep_time;
        sleep_time.tv_sec = (time_t)remaining;
        sleep_time.tv_nsec = (long)((remaining - sleep_time.tv_sec) * 1e9);
        while (nanosleep(&sleep_time, &sleep_time) != 0 && errno == EINTR) {
        }
    }
    pthread_mutex_unlock(&speed_limit_mutex);
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

static int sync_parent_directory(const char *path) {
    char directory[PATH_MAX];
    const char *slash = strrchr(path, '/');

    if (!slash) {
        strcpy(directory, ".");
    } else if (slash == path) {
        strcpy(directory, "/");
    } else {
        size_t length = (size_t)(slash - path);
        if (length >= sizeof(directory)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(directory, path, length);
        directory[length] = '\0';
    }

    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        return -1;
    }

    int result = fsync(directory_fd);
    int saved_errno = result == 0 ? 0 : errno;
    if (close(directory_fd) != 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }
    if (result != 0) {
        errno = saved_errno;
    }
    return result;
}

static int finalize_atomic_copy(int fd_dst, const char *tmp_dst, const char *dst) {
    if (fdatasync(fd_dst) != 0) {
        int saved_errno = errno;
        close(fd_dst);
        unlink(tmp_dst);
        fprintf(stderr, "fcp: cannot sync '%s': %s\n", tmp_dst, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    if (close(fd_dst) != 0) {
        int saved_errno = errno;
        unlink(tmp_dst);
        fprintf(stderr, "fcp: cannot close '%s': %s\n", tmp_dst, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    if (rename(tmp_dst, dst) != 0) {
        int saved_errno = errno;
        unlink(tmp_dst);
        fprintf(stderr, "fcp: atomic rename failed '%s' -> '%s': %s\n",
                tmp_dst, dst, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    if (sync_parent_directory(dst) != 0) {
        int saved_errno = errno;
        fprintf(stderr, "fcp: cannot sync destination directory for '%s': %s\n",
                dst, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    return 0;
}

/* Copy extended attributes between file descriptors or paths */
int fcp_copy_xattrs(int fd_src, int fd_dst, const char *src_path, const char *dst_path) {
    ssize_t list_len;

    if (fd_src >= 0) {
        list_len = flistxattr(fd_src, NULL, 0);
    } else if (src_path) {
        list_len = llistxattr(src_path, NULL, 0);
    } else {
        return 0;
    }

    if (list_len < 0) return -1;
    if (list_len == 0) return 0;

    char *list = malloc((size_t)list_len);
    if (!list) return -1;
    if (fd_src >= 0) {
        list_len = flistxattr(fd_src, list, (size_t)list_len);
    } else {
        list_len = llistxattr(src_path, list, (size_t)list_len);
    }
    if (list_len < 0) {
        free(list);
        return -1;
    }

    for (const char *name = list; name < list + list_len; name += strlen(name) + 1) {
        ssize_t val_len;
        if (fd_src >= 0) {
            val_len = fgetxattr(fd_src, name, NULL, 0);
        } else {
            val_len = lgetxattr(src_path, name, NULL, 0);
        }

        if (val_len < 0) {
            free(list);
            return -1;
        }
        void *value = malloc(val_len > 0 ? (size_t)val_len : 1);
        if (!value) {
            free(list);
            return -1;
        }
        if (fd_src >= 0) {
            val_len = fgetxattr(fd_src, name, value, (size_t)val_len);
        } else {
            val_len = lgetxattr(src_path, name, value, (size_t)val_len);
        }
        if (val_len < 0) {
            free(value);
            free(list);
            return -1;
        }

        int result;
        if (fd_dst >= 0) {
            result = fsetxattr(fd_dst, name, value, (size_t)val_len, 0);
        } else if (dst_path) {
            result = lsetxattr(dst_path, name, value, (size_t)val_len, 0);
        } else {
            result = -1;
            errno = EINVAL;
        }
        free(value);
        if (result != 0) {
            free(list);
            return -1;
        }
    }
    free(list);
    return 0;
}

/* Helper to apply metadata (permissions, ownership, timestamps, xattrs) */
static int apply_metadata(int fd_dst, const char *dst_path, const struct stat *src_stat,
                          int preserve_flags, int fd_src, const char *src_path) {
    if (preserve_flags & FCP_PRESERVE_OWNERSHIP) {
        int result = fd_dst >= 0 ? fchown(fd_dst, src_stat->st_uid, src_stat->st_gid) :
                     dst_path ? lchown(dst_path, src_stat->st_uid, src_stat->st_gid) : -1;
        if (result != 0) {
            return -1;
        }
    }

    if (preserve_flags & FCP_PRESERVE_MODE) {
        int result = fd_dst >= 0 ? fchmod(fd_dst, src_stat->st_mode) :
                     dst_path ? chmod(dst_path, src_stat->st_mode) : -1;
        if (result != 0) {
            return -1;
        }
    }

    if (preserve_flags & FCP_PRESERVE_XATTR) {
        if (fcp_copy_xattrs(fd_src, fd_dst, src_path, dst_path) != 0) {
            return -1;
        }
    }

    if (preserve_flags & FCP_PRESERVE_TIMESTAMPS) {
        struct timespec times[2];
        times[0] = src_stat->st_atim;
        times[1] = src_stat->st_mtim;
        int result = fd_dst >= 0 ? futimens(fd_dst, times) :
                     dst_path ? utimensat(AT_FDCWD, dst_path, times, AT_SYMLINK_NOFOLLOW) : -1;
        if (result != 0) {
            return -1;
        }
    }
    return 0;
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
        fd_dst = open(target_dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      src_stat.st_mode);
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
            if (apply_metadata(fd_dst, target_dst, &src_stat, preserve_flags, fd_src, src) != 0) {
                int saved_errno = errno;
                close(fd_dst);
                close(fd_src);
                if (atomic_mode) unlink(tmp_dst);
                fprintf(stderr, "fcp: cannot preserve metadata for '%s': %s\n", dst,
                        strerror(saved_errno));
                return FCP_COPY_ERROR;
            }
            if (atomic_mode) {
                if (finalize_atomic_copy(fd_dst, tmp_dst, dst) != 0) {
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
        if (apply_metadata(fd_dst, target_dst, &src_stat, preserve_flags, fd_src, src) != 0) {
            int saved_errno = errno;
            close(fd_dst);
            close(fd_src);
            if (atomic_mode) unlink(tmp_dst);
            fprintf(stderr, "fcp: cannot preserve metadata for '%s': %s\n", dst,
                    strerror(saved_errno));
            return FCP_COPY_ERROR;
        }
        if (atomic_mode) {
            if (finalize_atomic_copy(fd_dst, tmp_dst, dst) != 0) {
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
                        if (w <= 0) {
                            if (w < 0 && errno == EINTR) continue;
                            if (w == 0) errno = EIO;
                            seek_ok = false;
                            break;
                        }
                        w_total += w;
                    }
                    if (!seek_ok) break;

                    extent_bytes -= w_total;
                    sparse_copied += w_total;
                    limit_copy_speed(w_total, speed_limit);
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
        bool try_cfr = true;

        /* Update copy phase counters before rendering */
        if (progress && progress->enabled) {
            fcp_progress_update_copy(progress);
        }

        while (total_copied < file_size) {
            ssize_t chunk_size = 0;

            if (try_cfr) {
                size_t copy_length = file_size - total_copied;
                if (speed_limit > 0 && copy_length > FCP_BUFFER_SIZE) {
                    copy_length = FCP_BUFFER_SIZE;
                }
                ssize_t copied = copy_file_range(fd_src, NULL, fd_dst, NULL, copy_length, 0);
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
                    if (w <= 0) {
                        if (w < 0 && errno == EINTR) continue;
                        if (w == 0) errno = EIO;
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
            limit_copy_speed(chunk_size, speed_limit);
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
    if (apply_metadata(fd_dst, target_dst, &src_stat, preserve_flags, fd_src, src) != 0) {
        int saved_errno = errno;
        close(fd_src);
        close(fd_dst);
        free(buffer);
        if (atomic_mode) unlink(tmp_dst);
        fprintf(stderr, "fcp: cannot preserve metadata for '%s': %s\n", dst,
                strerror(saved_errno));
        return FCP_COPY_ERROR;
    }

    if (atomic_mode) {
        if (finalize_atomic_copy(fd_dst, tmp_dst, dst) != 0) {
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
