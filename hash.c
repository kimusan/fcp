/*
 * fcp - Faster CP
 * Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
 * MIT License
 */

#define _GNU_SOURCE

#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/evp.h>

/* Internal SHA256 hash function using EVP API */
static int compute_sha256(const char *path, char *hex_out, size_t hex_size) {
    int fd;
    FILE *fp;
    EVP_MD_CTX *ctx;
    const EVP_MD *md;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    unsigned char buffer[FCP_HASH_BLOCK_SIZE];
    size_t bytes_read;

    if (hex_size < FCP_HASH_HEX_LEN) {
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    fp = fdopen(fd, "rb");
    if (!fp) {
        close(fd);
        return -1;
    }

    md = EVP_sha256();
    ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(fp);
        return -1;
    }

    if (EVP_DigestInit_ex(ctx, md, NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        fclose(fp);
        return -1;
    }

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (EVP_DigestUpdate(ctx, buffer, bytes_read) != 1) {
            EVP_MD_CTX_free(ctx);
            fclose(fp);
            return -1;
        }
    }

    if (ferror(fp)) {
        EVP_MD_CTX_free(ctx);
        fclose(fp);
        return -1;
    }

    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        fclose(fp);
        return -1;
    }

    EVP_MD_CTX_free(ctx);
    fclose(fp);

    /* Convert to hex string */
    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(hex_out + (i * 2), "%02x", hash[i]);
    }
    hex_out[hash_len * 2] = '\0';

    return 0;
}

char *fcp_hash_file(const char *path) {
    char *hex = malloc(FCP_HASH_HEX_LEN);
    if (!hex) return NULL;

    if (compute_sha256(path, hex, FCP_HASH_HEX_LEN) != 0) {
        free(hex);
        return NULL;
    }

    return hex;
}

int fcp_hash_file_into(const char *path, char *hex_out, size_t hex_size) {
    return compute_sha256(path, hex_out, hex_size);
}

bool fcp_hash_compare(const char *hash1, const char *hash2) {
    return strcmp(hash1, hash2) == 0;
}