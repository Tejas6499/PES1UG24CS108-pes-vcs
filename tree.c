#include "pes.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    Tree sorted = *tree;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(TreeEntry), entry_cmp);
    size_t total = 0;
    for (int i = 0; i < sorted.count; i++)
        total += 8 + 1 + HASH_HEX_SIZE + 1 + strlen(sorted.entries[i].name) + 1;
    char *buf = malloc(total + 1);
    if (!buf) return -1;
    size_t pos = 0;
    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted.count; i++) {
        hash_to_hex(&sorted.entries[i].hash, hex);
        int n = snprintf(buf + pos, total + 1 - pos, "%o %s %s\n",
                         sorted.entries[i].mode, hex, sorted.entries[i].name);
        pos += (size_t)n;
    }
    *data_out = buf; *len_out = pos;
    return 0;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const char *p = (const char *)data, *end = p + len;
    while (p < end && tree_out->count < MAX_TREE_ENTRIES) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        TreeEntry *e = &tree_out->entries[tree_out->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned int mode;
        char name[256];
        if (sscanf(p, "%o %64s %255s", &mode, hex, name) == 3) {
            e->mode = mode;
            hex_to_hash(hex, &e->hash);
            strncpy(e->name, name, 255); e->name[255] = '\0';
            tree_out->count++;
        }
        p = nl + 1;
    }
    return 0;
}

/* Local index entry — mirrors index.h but avoids linking index.o */
typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} LocalEntry;

static int load_index_local(LocalEntry *entries, int *count) {
    *count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; /* empty index is fine */
    char line[1024];
    while (fgets(line, sizeof(line), f) && *count < 10000) {
        unsigned int mode;
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime;
        unsigned long size;
        char path[512];
        if (sscanf(line, "%o %64s %llu %lu %511s",
                   &mode, hex, &mtime, &size, path) == 5) {
            entries[*count].mode = mode;
            hex_to_hash(hex, &entries[*count].hash);
            strncpy(entries[*count].path, path, 511);
            entries[*count].path[511] = '\0';
            (*count)++;
        }
    }
    fclose(f);
    return 0;
}

static int build_subtree(const LocalEntry *entries, int count,
                          const char *prefix, int prefix_len,
                          ObjectID *id_out) {
    Tree tree; tree.count = 0;
    char done_dirs[256][512]; int done_count = 0;

    for (int i = 0; i < count; i++) {
        const char *path = entries[i].path;
        const char *rel;
        if (prefix_len == 0) {
            rel = path;
        } else {
            if (strncmp(path, prefix, (size_t)prefix_len) != 0 || path[prefix_len] != '/')
                continue;
            rel = path + prefix_len + 1;
        }
        const char *slash = strchr(rel, '/');
        if (!slash) {
            if (tree.count >= MAX_TREE_ENTRIES) continue;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, 255); te->name[255] = '\0';
        } else {
            int dir_len = (int)(slash - rel);
            char dir_name[256];
            strncpy(dir_name, rel, (size_t)dir_len); dir_name[dir_len] = '\0';
            int already = 0;
            for (int d = 0; d < done_count; d++)
                if (strcmp(done_dirs[d], dir_name) == 0) { already = 1; break; }
            if (already) continue;
            if (done_count < 256) { strncpy(done_dirs[done_count], dir_name, 255); done_dirs[done_count][255]='\0'; done_count++; }
            char sub_prefix[1024];
            if (prefix_len == 0) snprintf(sub_prefix, sizeof(sub_prefix), "%s", dir_name);
            else snprintf(sub_prefix, sizeof(sub_prefix), "%s/%s", prefix, dir_name);
            ObjectID sub_id;
            if (build_subtree(entries, count, sub_prefix, (int)strlen(sub_prefix), &sub_id) < 0) return -1;
            if (tree.count >= MAX_TREE_ENTRIES) continue;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = 0040000; te->hash = sub_id;
            strncpy(te->name, dir_name, 255); te->name[255] = '\0';
        }
    }
    void *serialized; size_t ser_size;
    if (tree_serialize(&tree, &serialized, &ser_size) < 0) return -1;
    int ret = object_write(OBJ_TREE, serialized, ser_size, id_out);
    free(serialized);
    return ret;
}

int tree_from_index(ObjectID *id_out) {
    LocalEntry *entries = malloc(10000 * sizeof(LocalEntry));
    if (!entries) return -1;
    int count = 0;
    load_index_local(entries, &count);
    int ret;
    if (count == 0) {
        Tree empty; empty.count = 0;
        void *s; size_t sz;
        tree_serialize(&empty, &s, &sz);
        ret = object_write(OBJ_TREE, s, sz, id_out);
        free(s);
    } else {
        ret = build_subtree(entries, count, "", 0, id_out);
    }
    free(entries);
    return ret;
}
