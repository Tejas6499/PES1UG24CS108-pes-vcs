#include "pes.h"
#include "commit.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

extern int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
extern int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

/* ── commit_serialize ─────────────────────────────────────────── */
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1], par_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit->tree,   tree_hex);
    hash_to_hex(&commit->parent, par_hex);

    char buf[8192]; int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "tree %s\n", tree_hex);
    if (commit->has_parent)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "parent %s\n", par_hex);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "author %s %llu\n",
                    commit->author, (unsigned long long)commit->timestamp);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "committer %s %llu\n",
                    commit->author, (unsigned long long)commit->timestamp);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n%s\n", commit->message);

    char *out = malloc((size_t)pos + 1);
    if (!out) return -1;
    memcpy(out, buf, (size_t)pos + 1);
    *data_out = out;
    *len_out  = (size_t)pos;
    return 0;
}

/* ── commit_parse ─────────────────────────────────────────────── */
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    memset(commit_out, 0, sizeof(*commit_out));
    const char *p = (const char *)data, *end = p + len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[4096]; if (ll >= sizeof(line)) ll = sizeof(line) - 1;
        memcpy(line, p, ll); line[ll] = '\0';

        if (strncmp(line, "tree ", 5) == 0) {
            hex_to_hash(line + 5, &commit_out->tree);
        } else if (strncmp(line, "parent ", 7) == 0) {
            hex_to_hash(line + 7, &commit_out->parent);
            commit_out->has_parent = 1;
        } else if (strncmp(line, "author ", 7) == 0) {
            const char *a = line + 7;
            const char *last = strrchr(a, ' ');
            if (last) {
                commit_out->timestamp = (uint64_t)strtoull(last + 1, NULL, 10);
                int al = (int)(last - a);
                strncpy(commit_out->author, a, (size_t)al);
                commit_out->author[al] = '\0';
            }
        } else if (ll == 0) {
            /* blank line: rest is message */
            p = nl ? nl + 1 : end;
            size_t ml = (size_t)(end - p);
            if (ml >= sizeof(commit_out->message)) ml = sizeof(commit_out->message) - 1;
            memcpy(commit_out->message, p, ml);
            commit_out->message[ml] = '\0';
            /* strip trailing newline */
            size_t mlen = strlen(commit_out->message);
            if (mlen > 0 && commit_out->message[mlen-1] == '\n')
                commit_out->message[mlen-1] = '\0';
            break;
        }
        p = nl ? nl + 1 : end;
    }
    return 0;
}

/* ── head_read ────────────────────────────────────────────────── */
int head_read(ObjectID *id_out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\n")] = '\0';

    char branch_path[512];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(branch_path, sizeof(branch_path), "%s/%s", PES_DIR, line + 5);
    } else {
        return hex_to_hash(line, id_out);
    }

    FILE *bf = fopen(branch_path, "r");
    if (!bf) return -1;
    char sha[HASH_HEX_SIZE + 2];
    if (!fgets(sha, sizeof(sha), bf)) { fclose(bf); return -1; }
    fclose(bf);
    sha[strcspn(sha, "\n")] = '\0';
    return hex_to_hash(sha, id_out);
}

/* ── head_update ──────────────────────────────────────────────── */
int head_update(const ObjectID *new_commit) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\n")] = '\0';

    char branch_path[512];
    if (strncmp(line, "ref: ", 5) == 0)
        snprintf(branch_path, sizeof(branch_path), "%s/%s", PES_DIR, line + 5);
    else
        snprintf(branch_path, sizeof(branch_path), "%s", HEAD_FILE);

    /* ensure parent dir exists */
    char dir[512]; strncpy(dir, branch_path, sizeof(dir)-1);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; mkdir(dir, 0755); }

    char tmp[512]; snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", branch_path);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    char hex[HASH_HEX_SIZE + 1]; hash_to_hex(new_commit, hex);
    dprintf(fd, "%s\n", hex);
    fsync(fd); close(fd);
    if (rename(tmp, branch_path) < 0) { unlink(tmp); return -1; }
    return 0;
}

/* ── commit_walk ──────────────────────────────────────────────── */
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID cur;
    if (head_read(&cur) != 0) return -1;

    int walked = 0;
    while (1) {
        void *data; size_t len; ObjectType type;
        if (object_read(&cur, &type, &data, &len) != 0) break;
        if (type != OBJ_COMMIT) { free(data); break; }
        Commit c; commit_parse(data, len, &c); free(data);
        callback(&cur, &c, ctx);
        walked++;
        if (!c.has_parent) break;
        cur = c.parent;
    }
    return (walked > 0) ? 0 : -1;
}

/* ── commit_create ────────────────────────────────────────────── */
int commit_create(const char *message, ObjectID *commit_id_out) {
    /* 1. Build tree from index */
    ObjectID tree_id;
    if (tree_from_index(&tree_id) != 0) {
        fprintf(stderr, "commit_create: tree_from_index failed\n");
        return -1;
    }

    /* 2. Read parent (may not exist for first commit) */
    Commit c; memset(&c, 0, sizeof(c));
    c.tree = tree_id;
    ObjectID parent_id;
    if (head_read(&parent_id) == 0) {
        c.parent = parent_id;
        c.has_parent = 1;
    }

    /* 3. Fill metadata */
    strncpy(c.author, pes_author(), sizeof(c.author) - 1);
    c.timestamp = (uint64_t)time(NULL);
    strncpy(c.message, message, sizeof(c.message) - 1);

    /* 4. Serialize and write */
    void *data; size_t data_len;
    if (commit_serialize(&c, &data, &data_len) != 0) return -1;
    if (object_write(OBJ_COMMIT, data, data_len, commit_id_out) != 0) { free(data); return -1; }
    free(data);

    /* 5. Update HEAD */
    return head_update(commit_id_out);
}
