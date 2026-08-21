/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#define _GNU_SOURCE

#include "util.h"
#include "copy.h"
#include "identical.h"
#include "progress.h"
#include "colors.h"
#include "queue.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <fnmatch.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <libgen.h>
#include <fcntl.h>

#define FCP_VERSION "1.0.0"

/* Global options */
static int opt_recursive = 0;
static int opt_interactive = 0;
static int opt_no_clobber = 0;
static int opt_force = 0;
static int opt_verbose = 0;
static int opt_dereference = 0;
static int opt_symbolic = 0;
static int opt_update = 0;
static int opt_dry_run = 0;
static int opt_parallel = 0;
static int opt_reflink = FCP_REFLINK_AUTO;
static uint64_t opt_speed_limit = 0;
static int opt_no_progress = 0;
static int opt_no_color = 0;
static int opt_verify_hash = 0;
static char *opt_target_dir = NULL;

/* Exclude patterns */
#define FCP_MAX_EXCLUDES 64
static char *opt_excludes[FCP_MAX_EXCLUDES];
static int opt_num_excludes = 0;

/* Progress state */
static fcp_progress_t g_progress;

/* Queue and workers */
static fcp_queue_t g_queue;
static pthread_t *g_workers = NULL;
static int g_num_workers = 0;

/* Scanning counter */
static int g_files_scanned = 0;
static int g_files_to_copy = 0;
static int g_files_skipped = 0;
static uint64_t g_bytes_skipped = 0;
static uint64_t g_bytes_total = 0;

/* Configuration */
static fcp_config_t g_config;

static const struct option long_options[] = {
    {"recursive",      no_argument,       NULL, 'r'},
    {"interactive",    no_argument,       NULL, 'i'},
    {"no-clobber",     no_argument,       NULL, 'n'},
    {"force",          no_argument,       NULL, 'f'},
    {"verbose",        no_argument,       NULL, 'v'},
    {"dereference",    no_argument,       NULL, 'd'},
    {"symbolic",       no_argument,       NULL, 's'},
    {"update",         no_argument,       NULL, 'u'},
    {"target-directory", required_argument, NULL, 't'},
    {"progress",       no_argument,       NULL, 'P'},
    {"no-progress",    no_argument,       NULL, 1004},
    {"parallel",       required_argument, NULL, 1005},
    {"verify-hash",    no_argument,       NULL, 1006},
    {"reflink",        required_argument, NULL, 1007},
    {"dry-run",        no_argument,       NULL, 1008},
    {"speed-limit",    required_argument, NULL, 1009},
    {"no-color",       no_argument,       NULL, 1010},
    {"config",         required_argument, NULL, 1011},
    {"help",           no_argument,       NULL, 'h'},
    {"version",        no_argument,       NULL, 'V'},
    {"exclude",        required_argument, NULL, 1014},
    {NULL, 0, NULL, 0}
};

static void print_version(void) {
    printf("fcp %s\n", FCP_VERSION);
}

static void print_help(FILE *fp) {
    fprintf(fp,
        "Usage: fcp [OPTION]... SOURCE DESTINATION\n"
        "       fcp [OPTION]... SOURCE... DIRECTORY\n"
        "\n"
        "fcp %s - Faster CP with progress, identical file detection, and parallelism\n"
        "\n"
        "Basic options:\n"
        "  -r, -R, --recursive      Copy directories recursively\n"
        "  -i, --interactive        Prompt before overwrite\n"
        "  -n, --no-clobber         Do not overwrite an existing file\n"
        "  -f, --force              Remove existing destination first\n"
        "  -v, --verbose            Display names of copied files\n"
        "  -d, --dereference        Copy files that symlinks refer to\n"
        "  -s, --symbolic           Create symlinks instead of copying\n"
        "  -u, --update             Copy only when source is newer\n"
        "  -t, --target-directory   Copy all sources into DIRECTORY\n"
        "\n"
        "fcp-specific options:\n"
        "  -P, --progress           Show progress bar (default: auto)\n"
        "      --no-progress        Disable progress display\n"
        "      --parallel=[N|auto]  Number of parallel copy workers (default: auto)\n"
        "      --exclude=PATTERN    Exclude files matching PATTERN (glob)\n"
        "      --verify-hash        Use SHA256 for identical file detection\n"
        "      --reflink=MODE       Use reflink when supported (auto|always|never)\n"
        "      --dry-run            Preview without copying\n"
        "      --speed-limit=SIZE   Cap copy speed (e.g., 10M, 1G)\n"
        "      --no-color           Disable colored output\n"
        "  -V, --version            Show version information\n"
        "  -h, --help               Show this help message\n"
        "\n"
        "Examples:\n"
        "  fcp source.txt destination.txt          Copy a file\n"
        "  fcp -r source_dir/ destination_dir/     Copy directory recursively\n"
        "  fcp --parallel=4 --progress file1 file2 dest/  Parallel copy with progress\n"
        "  fcp --dry-run -r dir1/ dir2/            Preview what would be copied\n"
        "  fcp --speed-limit=50M large_file.bin    Copy with 50MB/s speed limit\n"
        "\n"
        "Report bugs to: kim@schulz.dk\n",
        FCP_VERSION);
}

static bool prompt_interactive(const char *dst) {
    fprintf(stderr, "fcp: overwrite '%s'? ", dst);
    fflush(stderr);
    int c = getchar();
    bool overwrite = (c == 'y' || c == 'Y');
    while (c != '\n' && c != EOF) {
        c = getchar();
    }
    return overwrite;
}

/* Handle overwrite policy and identical file detection for a single file */
static int handle_overwrite(const char *src, const char *dst) {
    struct stat dst_stat;

    if (lstat(dst, &dst_stat) == 0) {
        /* If symbolic link creation mode (-s) */
        if (opt_symbolic) {
            if (opt_no_clobber) return 0;
            if (opt_interactive && !prompt_interactive(dst)) return 0;
            unlink(dst);
            return 1;
        }

        /* Destination exists - check if files are identical */
        int identical = fcp_check_identical(src, dst, opt_verify_hash);

        if (identical == FCP_IDENTICAL_YES) {
            if (opt_verbose) {
                fprintf(stderr, "fcp: skipped identical '%s'\n", src);
            }
            return 0; /* Skip - files are identical */
        }

        /* --update: skip if destination is newer or same age as source */
        if (opt_update) {
            struct stat src_stat;
            if (stat(src, &src_stat) == 0) {
                if (dst_stat.st_mtime >= src_stat.st_mtime) {
                    if (opt_verbose) {
                        fprintf(stderr, "fcp: skipped '%s' (destination is not older than source)\n", dst);
                    }
                    return 0;
                }
            }
        }

        /* --no-clobber: skip if dest exists (even if different) */
        if (opt_no_clobber) {
            if (opt_verbose) {
                fprintf(stderr, "fcp: skipped '%s' (file exists)\n", dst);
            }
            return 0;
        }

        /* --interactive: prompt user */
        if (opt_interactive) {
            if (!prompt_interactive(dst)) {
                return 0;
            }
        }

        /* --force: remove destination */
        if (opt_force) {
            if (unlink(dst) != 0 && errno != ENOENT) {
                fprintf(stderr, "fcp: cannot remove '%s': %s\n", dst, strerror(errno));
                return -1;
            }
        }
    }

    return 1;
}

/* Copy a single file */
static int copy_single_file(const char *src, const char *dst) {
    int allow = handle_overwrite(src, dst);
    if (allow < 0) return -1;
    if (allow == 0) return 0;

    if (opt_symbolic) {
        unlink(dst);
        if (symlink(src, dst) != 0) {
            fprintf(stderr, "fcp: cannot create symlink '%s' -> '%s': %s\n", dst, src, strerror(errno));
            return -1;
        }
        if (opt_verbose) {
            fprintf(stderr, "fcp: '%s' -> '%s'\n", dst, src);
        }
        return 0;
    }

    /* Get file size for progress */
    struct stat st;
    uint64_t file_size = 0;
    if (stat(src, &st) == 0) {
        file_size = st.st_size;
        g_bytes_total += file_size;
        fcp_progress_update_file(&g_progress, dst, file_size);
    }

    int ret = fcp_copy_file(src, dst, opt_reflink, opt_dry_run, opt_speed_limit, &g_progress);

    if (ret == FCP_COPY_OK) {
        fcp_progress_complete_file(&g_progress);
        if (opt_verbose) {
            fprintf(stderr, "fcp: copied '%s' -> '%s'\n", src, dst);
        }
    } else if (ret == FCP_COPY_SKIP) {
        fcp_progress_complete_file(&g_progress);
        if (opt_verbose) {
            fprintf(stderr, "fcp: skipped identical '%s'\n", src);
        }
    }

    return (ret == FCP_COPY_OK) ? 0 : (ret == FCP_COPY_SKIP) ? 0 : -1;
}

/* Worker thread function */
static void *worker_thread(void *arg) {
    (void)arg;
    fcp_queue_item_t item;

    while (fcp_queue_pop(&g_queue, &item) == 0) {
        int allow = handle_overwrite(item.src, item.dst);
        if (allow <= 0) {
            if (allow == 0) {
                g_files_skipped++;
                g_bytes_skipped += item.size;
                if (g_progress.enabled) {
                    g_progress.total_done += item.size;
                }
            }
            free(item.src);
            free(item.dst);
            continue;
        }

        if (opt_symbolic) {
            unlink(item.dst);
            if (symlink(item.src, item.dst) != 0) {
                fprintf(stderr, "fcp: cannot create symlink '%s' -> '%s': %s\n", item.dst, item.src, strerror(errno));
            } else if (opt_verbose) {
                fprintf(stderr, "fcp: '%s' -> '%s'\n", item.dst, item.src);
            }
            free(item.src);
            free(item.dst);
            continue;
        }

        /* Update progress */
        if (g_progress.enabled) {
            fcp_progress_update_file(&g_progress, item.dst, item.size);
        }

        /* Copy file */
        int ret = fcp_copy_file(item.src, item.dst, opt_reflink, opt_dry_run,
                               opt_speed_limit, &g_progress);

        if (ret == FCP_COPY_OK) {
            g_files_to_copy++;
            fcp_progress_complete_file(&g_progress);
            if (opt_verbose) {
                fprintf(stderr, "fcp: copied '%s' -> '%s'\n", item.src, item.dst);
            }
        } else if (ret == FCP_COPY_SKIP) {
            fcp_progress_complete_file(&g_progress);
            if (opt_verbose) {
                fprintf(stderr, "fcp: skipped identical '%s'\n", item.src);
            }
        }

        free(item.src);
        free(item.dst);
    }

    return NULL;
}

/* Check if a filename matches any exclude pattern */
static int is_excluded(const char *name) {
    if (opt_num_excludes == 0) return 0;
    for (int i = 0; i < opt_num_excludes; i++) {
        if (fnmatch(opt_excludes[i], name, FNM_PERIOD) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Recursively copy a directory (populates queue) */
static int copy_directory(const char *src, const char *dst) {
    struct dirent *de;
    DIR *dir;
    char *src_path, *dst_path;
    char clean_dst[1024];

    /* Strip trailing slash from dst */
    strncpy(clean_dst, dst, sizeof(clean_dst) - 1);
    clean_dst[sizeof(clean_dst) - 1] = '\0';
    size_t dst_len = strlen(clean_dst);
    while (dst_len > 1 && clean_dst[dst_len - 1] == '/') {
        clean_dst[dst_len - 1] = '\0';
        dst_len--;
    }

    /* Ensure destination exists (skip if dst already exists as directory) */
    if (mkdir(clean_dst, 0755) != 0 && errno != EEXIST) {
        /* If it's an existing directory, that's fine */
        struct stat st;
        if (stat(clean_dst, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "fcp: cannot create directory '%s': %s\n", clean_dst, strerror(errno));
            return -1;
        }
    }

    dir = opendir(src);
    if (!dir) {
        fprintf(stderr, "fcp: cannot open directory '%s': %s\n", src, strerror(errno));
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        /* Skip excluded patterns */
        if (is_excluded(de->d_name)) {
            continue;
        }

        src_path = malloc(strlen(src) + strlen(de->d_name) + 2);
        sprintf(src_path, "%s/%s", src, de->d_name);

        dst_path = malloc(strlen(clean_dst) + strlen(de->d_name) + 2);
        sprintf(dst_path, "%s/%s", clean_dst, de->d_name);

        struct stat st;
        if (lstat(src_path, &st) != 0) {
            fprintf(stderr, "fcp: cannot stat '%s': %s\n", src_path, strerror(errno));
            free(src_path);
            free(dst_path);
            continue;
        }

        g_files_scanned++;
        g_bytes_total += st.st_size;
        if (g_progress.enabled && g_progress.phase == FCP_PHASE_SCANNING) {
            fcp_progress_update_scanning(&g_progress, g_files_scanned, g_bytes_skipped, g_files_skipped, g_bytes_total);
            /* Render scanning progress from main thread */
            fcp_progress_render(&g_progress);
        }

        if (S_ISDIR(st.st_mode)) {
            copy_directory(src_path, dst_path);
        } else if (S_ISREG(st.st_mode)) {
            /* Check if identical first */
            int identical = fcp_check_identical(src_path, dst_path, opt_verify_hash);
            if (identical == FCP_IDENTICAL_YES) {
                g_files_skipped++;
                g_bytes_skipped += st.st_size;
                /* Count skipped files in progress (they're "instantly copied") */
                if (g_progress.enabled) {
                    g_progress.total_all += st.st_size;
                    g_progress.total_done += st.st_size;
                }
                /* Update scanning progress to show skipped files */
                fcp_progress_update_scanning(&g_progress, g_files_scanned, g_bytes_skipped, g_files_skipped, g_bytes_total);
                if (opt_verbose) {
                    fprintf(stderr, "fcp: skipped identical '%s'\n", src_path);
                }
            } else {
                g_files_to_copy++;
                /* Add to queue for parallel copying */
                if (opt_parallel > 1) {
                    fcp_queue_push(&g_queue, src_path, dst_path, st.st_size);
                } else {
                    /* Sequential: copy directly */
                    if (g_progress.enabled) {
                        fcp_progress_update_file(&g_progress, dst_path, st.st_size);
                    }

                    int ret = fcp_copy_file(src_path, dst_path, opt_reflink, opt_dry_run,
                                           opt_speed_limit, &g_progress);

                    if (ret == FCP_COPY_OK) {
                        fcp_progress_complete_file(&g_progress);
                        if (opt_verbose) {
                            fprintf(stderr, "fcp: copied '%s' -> '%s'\n", src_path, dst_path);
                        }
                    } else if (ret == FCP_COPY_SKIP) {
                        fcp_progress_complete_file(&g_progress);
                        if (opt_verbose) {
                            fprintf(stderr, "fcp: skipped identical '%s'\n", src_path);
                        }
                    }
                }
            }
        } else if (S_ISLNK(st.st_mode)) {
            if (opt_dereference) {
                struct stat res_stat;
                if (stat(src_path, &res_stat) == 0) {
                    if (S_ISDIR(res_stat.st_mode)) {
                        copy_directory(src_path, dst_path);
                    } else if (S_ISREG(res_stat.st_mode)) {
                        if (opt_parallel > 1) {
                            fcp_queue_push(&g_queue, src_path, dst_path, res_stat.st_size);
                        } else {
                            copy_single_file(src_path, dst_path);
                        }
                    }
                }
            } else {
                /* Copy symlink directly */
                char target[4096];
                ssize_t len = readlink(src_path, target, sizeof(target) - 1);
                if (len >= 0) {
                    target[len] = '\0';
                    unlink(dst_path);
                    if (symlink(target, dst_path) != 0) {
                        fprintf(stderr, "fcp: cannot create symlink '%s': %s\n", dst_path, strerror(errno));
                    } else if (opt_verbose) {
                        fprintf(stderr, "fcp: '%s' -> '%s'\n", dst_path, target);
                    }
                }
            }
        }

        free(src_path);
        free(dst_path);
    }

    closedir(dir);

    /* Preserve directory permissions and timestamps */
    struct stat dir_stat;
    if (stat(src, &dir_stat) == 0) {
        chmod(clean_dst, dir_stat.st_mode);
        struct timespec dtimes[2];
        dtimes[0] = dir_stat.st_atim;
        dtimes[1] = dir_stat.st_mtim;
        utimensat(AT_FDCWD, clean_dst, dtimes, 0);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    int opt;
    int option_index = 0;

    /* Load config file */
    fcp_config_defaults(&g_config);
    const char *home = getenv("HOME");
    if (home) {
        char config_path[2048];
        snprintf(config_path, sizeof(config_path), "%s/.config/fcp/config", home);
        fcp_config_load(&g_config, config_path);
    }

    while ((opt = getopt_long(argc, argv, "rRiInfvdsut:PhV", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'r':
            case 'R':
                opt_recursive = 1;
                break;
            case 'i':
                opt_interactive = 1;
                break;
            case 'n':
                opt_no_clobber = 1;
                break;
            case 'f':
                opt_force = 1;
                break;
            case 'v':
                opt_verbose = 1;
                break;
            case 'd':
                opt_dereference = 1;
                break;
            case 's':
                opt_symbolic = 1;
                break;
            case 'u':
                opt_update = 1;
                break;
            case 't':
                opt_target_dir = optarg;
                break;
            case 'P':
            case 1003: /* --progress */
                opt_no_progress = 0;
                break;
            case 1004: /* --no-progress */
                opt_no_progress = 1;
                break;
            case 1005: /* --parallel */
                if (strcmp(optarg, "auto") == 0) {
                    opt_parallel = 0;
                } else {
                    opt_parallel = atoi(optarg);
                }
                break;
            case 1006: /* --verify-hash */
                opt_verify_hash = 1;
                break;
            case 1007: /* --reflink */
                if (strcmp(optarg, "auto") == 0) {
                    opt_reflink = FCP_REFLINK_AUTO;
                } else if (strcmp(optarg, "always") == 0) {
                    opt_reflink = FCP_REFLINK_ALWAYS;
                } else if (strcmp(optarg, "never") == 0 || strcmp(optarg, "off") == 0) {
                    opt_reflink = FCP_REFLINK_OFF;
                } else {
                    fprintf(stderr, "fcp: invalid reflink mode '%s'\n", optarg);
                    return 1;
                }
                break;
            case 1008: /* --dry-run */
                opt_dry_run = 1;
                break;
            case 1009: /* --speed-limit */
                if (parse_size(optarg, &opt_speed_limit) != 0) {
                    fprintf(stderr, "fcp: invalid speed limit '%s'\n", optarg);
                    return 1;
                }
                break;
            case 1010: /* --no-color */
                opt_no_color = 1;
                break;
            case 1011: /* --config */
                fcp_config_load(&g_config, optarg);
                break;
case 'h':
            print_help(stdout);
            return 0;
        case 'V':
            print_version();
            return 0;
        case 1012: /* --help */
            print_help(stdout);
            return 0;
        case 1013: /* --version */
            print_version();
            return 0;
            case 1014: /* --exclude */
                if (opt_num_excludes < FCP_MAX_EXCLUDES) {
                    opt_excludes[opt_num_excludes++] = optarg;
                }
                break;
            default:
                print_help(stderr);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "fcp: missing file operand\n");
        print_help(stderr);
        return 1;
    }

    /* Need at least 2 arguments for source and destination */
    if (optind + 1 >= argc && !opt_target_dir) {
        if (!opt_recursive) {
            fprintf(stderr, "fcp: missing destination operand\n");
            print_help(stderr);
            return 1;
        }
    }

    /* Initialize progress */
    bool progress_enabled = !opt_no_progress && (!opt_dry_run || opt_verbose);
    fcp_progress_init(&g_progress, progress_enabled);

    /* Print startup banner */
    if (optind < argc) {
        const char *first_arg = argv[optind];
        const char *banner_src = first_arg;
        const char *banner_dst = "";
        if (optind + 1 < argc) {
            banner_dst = argv[optind + 1];
        }
        fcp_progress_banner(&g_progress, banner_src, banner_dst);
    }

    /* Determine number of workers */
    if (opt_parallel == 0) {
        /* Auto: use nproc, capped at 8 */
        g_num_workers = get_num_cores();
        if (g_num_workers > 8) g_num_workers = 8;
        if (g_num_workers < 1) g_num_workers = 1;
    } else if (opt_parallel == 1) {
        g_num_workers = 1;
    } else {
        g_num_workers = opt_parallel;
    }

    /* Initialize queue and worker threads */
    fcp_queue_init(&g_queue, g_num_workers * 4);

    if (g_num_workers > 1) {
        g_workers = malloc(sizeof(pthread_t) * g_num_workers);
        if (!g_workers) {
            perror("fcp: malloc");
            return 1;
        }

        for (int i = 0; i < g_num_workers; i++) {
            if (pthread_create(&g_workers[i], NULL, worker_thread, NULL) != 0) {
                perror("fcp: pthread_create");
                return 1;
            }
        }
    }

    /* If target directory specified, copy all sources into it */
    if (opt_target_dir) {
        struct stat target_stat;
        if (stat(opt_target_dir, &target_stat) != 0 || !S_ISDIR(target_stat.st_mode)) {
            fprintf(stderr, "fcp: '%s' is not a directory\n", opt_target_dir);
            goto cleanup;
        }

        /* Start scanning phase */
        if (g_progress.enabled) {
            g_files_scanned = 0;
            g_files_to_copy = 0;
            g_files_skipped = 0;
            g_bytes_skipped = 0;
            g_bytes_total = 0;
            fcp_progress_set_scanning(&g_progress, 0);
        }

        for (int i = optind; i < argc; i++) {
            char *dst = malloc(strlen(opt_target_dir) + strlen(argv[i]) + 2);
            const char *basename = strrchr(argv[i], '/');
            basename = basename ? basename + 1 : argv[i];
            sprintf(dst, "%s/%s", opt_target_dir, basename);

            if (opt_recursive && path_is_dir(argv[i])) {
                copy_directory(argv[i], dst);
            } else {
                if (g_progress.enabled && g_progress.phase == FCP_PHASE_SCANNING) {
                    g_files_to_copy++;
                }
                copy_single_file(argv[i], dst);
            }

            free(dst);
        }

        if (g_progress.enabled) {
            g_progress.total_all = g_bytes_total;
            fcp_progress_scanning_done(&g_progress, g_files_skipped, g_files_to_copy);
        }
    } else if (argc - optind > 2) {
        /* Multiple sources - destination must be directory */
        const char *dst = argv[argc - 1];

        struct stat dst_stat;
        if (stat(dst, &dst_stat) != 0 || !S_ISDIR(dst_stat.st_mode)) {
            fprintf(stderr, "fcp: target '%s' is not a directory\n", dst);
            goto cleanup;
        }

        if (g_progress.enabled) {
            g_files_scanned = 0;
            g_files_to_copy = 0;
            g_files_skipped = 0;
            fcp_progress_set_scanning(&g_progress, 0);
        }

        for (int i = optind; i < argc - 1; i++) {
            char *full_dst = malloc(strlen(dst) + strlen(argv[i]) + 2);
            const char *basename = strrchr(argv[i], '/');
            basename = basename ? basename + 1 : argv[i];
            sprintf(full_dst, "%s/%s", dst, basename);

            if (opt_recursive && path_is_dir(argv[i])) {
                copy_directory(argv[i], full_dst);
            } else {
                if (g_progress.enabled && g_progress.phase == FCP_PHASE_SCANNING) {
                    g_files_to_copy++;
                }
                copy_single_file(argv[i], full_dst);
            }

            free(full_dst);
        }

        if (g_progress.enabled) {
            g_progress.total_all = g_bytes_total;
            fcp_progress_scanning_done(&g_progress, g_files_skipped, g_files_to_copy);
        }
    } else if (argc - optind == 2) {
        /* Single source, single destination */
        const char *src = argv[optind];
        const char *dst = argv[optind + 1];

        if (opt_recursive && path_is_dir(src)) {
            if (g_progress.enabled) {
                g_files_scanned = 0;
                g_files_to_copy = 0;
                g_files_skipped = 0;
                g_bytes_skipped = 0;
                g_bytes_total = 0;
                fcp_progress_set_scanning(&g_progress, 0);
            }
            copy_directory(src, dst);
            if (g_progress.enabled) {
                g_progress.total_all = g_bytes_total;
                fcp_progress_scanning_done(&g_progress, g_files_skipped, g_files_to_copy);
                fcp_progress_render(&g_progress);
            }
        } else {
            /* For single file, just copy directly */
            char *final_dst = NULL;
            if (path_is_dir(dst)) {
                const char *basename = strrchr(src, '/');
                basename = basename ? basename + 1 : src;
                final_dst = malloc(strlen(dst) + strlen(basename) + 2);
                sprintf(final_dst, "%s/%s", dst, basename);
            } else {
                final_dst = strdup(dst);
            }

            struct stat st;
            if (stat(src, &st) == 0) {
                if (g_num_workers > 1) {
                    fcp_queue_push(&g_queue, src, final_dst, st.st_size);
                } else {
                    copy_single_file(src, final_dst);
                }
            }
            free(final_dst);
        }
    }

cleanup:
    /* Mark queue as done and wait for workers */
    if (g_num_workers > 1 && g_workers) {
        fcp_queue_mark_done(&g_queue);
        for (int i = 0; i < g_num_workers; i++) {
            pthread_join(g_workers[i], NULL);
        }
        free(g_workers);
        g_workers = NULL;
    }

    /* Print summary and cleanup progress */
    if (g_progress.enabled) {
        fcp_progress_summary(&g_progress);
        fcp_progress_cleanup(&g_progress);
    }
    fcp_queue_cleanup(&g_queue);

    return 0;
}