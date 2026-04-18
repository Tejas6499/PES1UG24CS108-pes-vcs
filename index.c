#include "pes.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <openssl/evp.h>

extern int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

IndexEntry *index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++)
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            memmove(&index->entries[i], &index->entries[i+1],
                    (size_t)(index->count - i - 1) * sizeof(IndexEntry));
            index->count--;
            return 0;
        }
    }
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    if (index->count == 0) printf("  (nothing to show)\n");
    else for (int i = 0; i < index->count; i++) printf("  staged:     %s\n", index->entries[i].path);
    printf("\n");
    printf("Unstaged changes:\n");
    int unstaged = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) { printf("  deleted:    %s\n", index->entries[i].path); unstaged++; }
        else {
            FILE *f = fopen(index->entries[i].path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
                char *buf = malloc((size_t)sz + 1);
                if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                    char hdr[64]; int hl = snprintf(hdr, sizeof(hdr), "blob %ld", sz);
                    size_t tot = (size_t)hl + 1 + (size_t)sz;
                    char *full = malloc(tot);
                    if (full) {
                        memcpy(full, hdr, (size_t)hl + 1); memcpy(full + hl + 1, buf, (size_t)sz);
                        ObjectID d; unsigned int dl;
                        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
                        EVP_DigestUpdate(ctx, full, tot);
                        EVP_DigestFinal_ex(ctx, d.hash, &dl);
                        EVP_MD_CTX_free(ctx); free(full);
                        if (memcmp(d.hash, index->entries[i].hash.hash, HASH_SIZE) != 0) { printf("  modified:   %s\n", index->entries[i].path); unstaged++; }
                    }
                }
                free(buf); fclose(f);
            }
        }
    }
    if (unstaged == 0) printf("  (nothing to show)\n");
    printf("\n");
    printf("Untracked files:\n");
    int untracked = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            struct stat st;
            if (stat(ent->d_name, &st) != 0 || S_ISDIR(st.st_mode)) continue;
            int found = 0;
            for (int i = 0; i < index->count; i++)
                if (strcmp(index->entries[i].path, ent->d_name) == 0) { found = 1; break; }
            if (!found) { printf("  untracked:  %s\n", ent->d_name); untracked++; }
        }
        closedir(dir);
    }
    if (untracked == 0) printf("  (nothing to show)\n");
    return 0;
}

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) { if (errno == ENOENT) return 0; return -1; }
    char line[1024];
    while (fgets(line, sizeof(line), f) && index->count < MAX_INDEX_ENTRIES) {
        unsigned int mode; char hex[65]; unsigned long long mtime; unsigned long size; char path[512];
        if (sscanf(line, "%o %64s %llu %lu %511s", &mode, hex, &mtime, &size, path) == 5) {
            IndexEntry *e = &index->entries[index->count++];
            e->mode = mode; hex_to_hash(hex, &e->hash);
            e->mtime_sec = mtime; e->size = (uint32_t)size;
            strncpy(e->path, path, 511); e->path[511] = 0;
        }
    }
    fclose(f); return 0;
}

static int ptr_cmp(const void *a, const void *b) {
    return strcmp((*(const IndexEntry **)a)->path, (*(const IndexEntry **)b)->path);
}

int index_save(const Index *index) {
    const IndexEntry **ptrs = malloc((size_t)(index->count + 1) * sizeof(IndexEntry *));
    if (!ptrs) return -1;
    for (int i = 0; i < index->count; i++) ptrs[i] = &index->entries[i];
    if (index->count > 0) qsort(ptrs, (size_t)index->count, sizeof(IndexEntry *), ptr_cmp);
    char tmp[256]; snprintf(tmp, sizeof(tmp), "%s/index.tmp.XXXXXX", PES_DIR);
    int fd = mkstemp(tmp);
    if (fd < 0) { free(ptrs); return -1; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); free(ptrs); return -1; }
    char hex[65];
    for (int i = 0; i < index->count; i++) {
        hash_to_hex(&ptrs[i]->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n", ptrs[i]->mode, hex,
                (unsigned long long)ptrs[i]->mtime_sec, ptrs[i]->size, ptrs[i]->path);
    }
    free(ptrs); fflush(f); fsync(fileno(f)); fclose(f);
    if (rename(tmp, INDEX_FILE) < 0) { unlink(tmp); return -1; }
    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "index_add: cannot stat '%s'\n", path); return -1; }
    if (!S_ISREG(st.st_mode)) { fprintf(stderr, "index_add: not a regular file\n"); return -1; }
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *content = malloc((size_t)sz + 1);
    if (!content) { fclose(f); return -1; }
    if (sz > 0 && fread(content, 1, (size_t)sz, f) != (size_t)sz) { free(content); fclose(f); return -1; }
    fclose(f);
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, content, (size_t)sz, &blob_id) != 0) { free(content); return -1; }
    free(content);
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    IndexEntry *e = index_find(index, path);
    if (!e) { if (index->count >= MAX_INDEX_ENTRIES) return -1; e = &index->entries[index->count++]; }
    e->mode = mode; e->hash = blob_id;
    e->mtime_sec = (uint64_t)st.st_mtime; e->size = (uint32_t)st.st_size;
    strncpy(e->path, path, 511); e->path[511] = 0;
    return index_save(index);
}
