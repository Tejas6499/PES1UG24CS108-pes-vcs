#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/evp.h>

/* ── PROVIDED ─────────────────────────────────────────────────── */

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++)
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

/* ── TODO: object_write ───────────────────────────────────────── */

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    /* Build header: "type size\0" */
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t header_size = (size_t)hlen + 1; /* +1 for null byte */

    /* Combine header + data into one buffer */
    size_t total = header_size + len;
    char *full = malloc(total);
    if (!full) return -1;
    memcpy(full, header, header_size);
    memcpy(full + header_size, data, len);

    /* Compute hash of full object */
    compute_hash(full, total, id_out);

    /* Deduplication: if it already exists we're done */
    if (object_exists(id_out)) { free(full); return 0; }

    /* Create shard directory */
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755);

    /* Get final path */
    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));

    /* Write to temp file */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_XXXXXX", shard_dir);
    int fd = mkstemp(tmp_path);
    if (fd < 0) { free(full); return -1; }

    size_t written = 0;
    while (written < total) {
        ssize_t n = write(fd, (char *)full + written, total - written);
        if (n < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp_path); free(full); return -1; }
        written += (size_t)n;
    }
    free(full);

    /* fsync file, rename, fsync directory */
    fsync(fd);
    close(fd);
    if (rename(tmp_path, final_path) < 0) { unlink(tmp_path); return -1; }
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    return 0;
}

/* ── TODO: object_read ────────────────────────────────────────── */

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 0) { fclose(f); return -1; }
    rewind(f);

    char *raw = malloc((size_t)file_size);
    if (!raw) { fclose(f); return -1; }
    if (fread(raw, 1, (size_t)file_size, f) != (size_t)file_size) { fclose(f); free(raw); return -1; }
    fclose(f);

    /* Integrity check */
    ObjectID computed;
    compute_hash(raw, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        fprintf(stderr, "object_read: integrity check failed\n");
        free(raw); return -1;
    }

    /* Find null separator between header and data */
    char *sep = memchr(raw, '\0', (size_t)file_size);
    if (!sep) { free(raw); return -1; }

    /* Parse type */
    if (type_out) {
        if      (strncmp(raw, "blob ",   5) == 0) *type_out = OBJ_BLOB;
        else if (strncmp(raw, "tree ",   5) == 0) *type_out = OBJ_TREE;
        else if (strncmp(raw, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
        else { free(raw); return -1; }
    }

    /* Parse size */
    char *space = memchr(raw, ' ', (size_t)(sep - raw));
    if (!space) { free(raw); return -1; }
    size_t data_len = (size_t)atol(space + 1);

    /* Copy payload */
    size_t header_len = (size_t)(sep - raw) + 1;
    char *payload = malloc(data_len + 1);
    if (!payload) { free(raw); return -1; }
    memcpy(payload, raw + header_len, data_len);
    payload[data_len] = '\0';

    *data_out = payload;
    if (len_out) *len_out = data_len;
    free(raw);
    return 0;
}
