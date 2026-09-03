/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#define CONFIG_MAX_LINE 1024

void fcp_config_defaults(fcp_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->parallel = 0; /* auto */
    config->progress_auto = true;
    config->color_auto = true;
    config->verify_hash = false;
    config->verbose_auto = true;
    config->reflink = 1; /* auto */
    config->sparse = 1;  /* auto */
    config->atomic = false;
    config->speed_limit = 0;
    strcpy(config->config_path, "");
}

static char *skip_whitespace(char *str) {
    while (*str && isspace(*str)) str++;
    return str;
}

static char *trim_newline(char *str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len-1] == '\n' || str[len-1] == '\r')) {
        str[len-1] = '\0';
        len--;
    }
    return str;
}

int fcp_config_load(fcp_config_t *config, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[CONFIG_MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        char *str = skip_whitespace(line);
        str = trim_newline(str);

        /* Skip comments and empty lines */
        if (*str == '#' || *str == '\0') {
            continue;
        }

        /* Parse key = value */
        char *eq = strchr(str, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = str;
        char *value = eq + 1;
        key = skip_whitespace(key);

        /* Trim key */
        size_t key_len = strlen(key);
        while (key_len > 0 && isspace(key[key_len-1])) {
            key[key_len-1] = '\0';
            key_len--;
        }

        value = skip_whitespace(value);

        if (strcmp(key, "parallel") == 0) {
            if (parse_parallel_count(value, &config->parallel) != 0) {
                fclose(fp);
                errno = EINVAL;
                return -1;
            }
        } else if (strcmp(key, "progress") == 0) {
            if (strcmp(value, "off") == 0 || strcmp(value, "false") == 0) {
                config->progress_auto = false;
            }
        } else if (strcmp(key, "color") == 0) {
            if (strcmp(value, "off") == 0 || strcmp(value, "false") == 0) {
                config->color_auto = false;
            }
        } else if (strcmp(key, "verify_hash") == 0) {
            if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0) {
                config->verify_hash = true;
            }
        } else if (strcmp(key, "verbose") == 0) {
            if (strcmp(value, "off") == 0 || strcmp(value, "false") == 0) {
                config->verbose_auto = false;
            }
        } else if (strcmp(key, "reflink") == 0) {
            if (strcmp(value, "always") == 0) {
                config->reflink = 2;
            } else if (strcmp(value, "never") == 0 || strcmp(value, "off") == 0) {
                config->reflink = 0;
            }
        } else if (strcmp(key, "sparse") == 0) {
            if (strcmp(value, "always") == 0) {
                config->sparse = 2;
            } else if (strcmp(value, "never") == 0 || strcmp(value, "off") == 0) {
                config->sparse = 0;
            } else {
                config->sparse = 1;
            }
        } else if (strcmp(key, "atomic") == 0) {
            if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                config->atomic = true;
            } else {
                config->atomic = false;
            }
        } else if (strcmp(key, "speed_limit") == 0) {
            /* Parse size string */
            char *endptr;
            double val = strtod(value, &endptr);
            uint64_t mult = 1;
            if (*endptr == 'K' || *endptr == 'k') { mult = 1024; endptr++; }
            else if (*endptr == 'M' || *endptr == 'm') { mult = 1024*1024; endptr++; }
            else if (*endptr == 'G' || *endptr == 'g') { mult = 1024*1024*1024; endptr++; }
            if (*endptr == '\0') {
                config->speed_limit = (uint64_t)(val * mult);
            }
        }
    }

    fclose(fp);
    return 0;
}

void fcp_config_apply_overrides(fcp_config_t *config,
                                int parallel, bool progress, bool color,
                                bool verify_hash, bool verbose,
                                int reflink, uint64_t speed_limit) {
    /* CLI flags override config values when explicitly set */
    /* This is a simplified implementation - in practice, you'd track which flags were explicitly set */
    if (parallel > 0) config->parallel = parallel;
    if (!progress) config->progress_auto = false;
    if (!color) config->color_auto = false;
    if (verify_hash) config->verify_hash = true;
    if (!verbose) config->verbose_auto = false;
    if (reflink != 1) config->reflink = reflink;
    if (speed_limit > 0) config->speed_limit = speed_limit;
}
