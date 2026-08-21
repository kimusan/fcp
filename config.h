/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#ifndef FCP_CONFIG_H
#define FCP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Config values */
typedef struct {
    int parallel;        /* 0 = auto, 1 = sequential, >1 = N workers */
    bool progress_auto;  /* true = auto-detect, false = explicit off */
    bool color_auto;     /* true = auto-detect, false = explicit off */
    bool verify_hash;    /* true = always verify with SHA256 */
    bool verbose_auto;   /* true = auto-detect, false = explicit off */
    int reflink;         /* 0 = off, 1 = auto, 2 = always */
    int sparse;          /* 0 = off, 1 = auto, 2 = always */
    bool atomic;         /* true = atomic replacement */
    uint64_t speed_limit; /* 0 = no limit */
    char config_path[1024]; /* Path to config file */
} fcp_config_t;

/* Load config from file. Returns 0 on success. */
int fcp_config_load(fcp_config_t *config, const char *path);

/* Get default config values */
void fcp_config_defaults(fcp_config_t *config);

/* Apply CLI overrides to config (CLI takes precedence) */
void fcp_config_apply_overrides(fcp_config_t *config,
                                int parallel, bool progress, bool color,
                                bool verify_hash, bool verbose,
                                int reflink, uint64_t speed_limit);

#endif /* FCP_CONFIG_H */