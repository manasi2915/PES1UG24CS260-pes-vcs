#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pes.h"
// helper
static int cmp_tree(const void *a, const void *b) {
    return strcmp(((TreeEntry *)a)->name,
                  ((TreeEntry *)b)->name);
}

// ─────────────────────────────────────────────
// tree_serialize
// ─────────────────────────────────────────────
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    char buf[8192];
    int n = 0;

    Tree tmp = *tree;
    qsort(tmp.entries, tmp.count, sizeof(TreeEntry), cmp_tree);

    for (int i = 0; i < tmp.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&tmp.entries[i].hash, hex);

        n += snprintf(buf + n, sizeof(buf) - n,
                      "%06o %s %s\n",
                      tmp.entries[i].mode,
                      hex,
                      tmp.entries[i].name);
    }

    *data_out = malloc(n + 1);
    memcpy(*data_out, buf, n + 1);
    *len_out = n;
    return 0;
}

// ─────────────────────────────────────────────
// tree_parse (simple, not heavily used here)
// ─────────────────────────────────────────────
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    const char *p = data;
    tree_out->count = 0;

    while ((size_t)(p - (char *)data) < len) {
        TreeEntry e;
        char hex[HASH_HEX_SIZE + 1];

        if (sscanf(p, "%o %64s %255s",
                   &e.mode,
                   hex,
                   e.name) != 3)
            break;

        hex_to_hash(hex, &e.hash);
        tree_out->entries[tree_out->count++] = e;

        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }

    return 0;
}

// ─────────────────────────────────────────────
// tree_from_index (CORE FUNCTION)
// ─────────────────────────────────────────────
int tree_from_index(ObjectID *id_out) {
    Index index;
    index_load(&index);

    Tree root;
    root.count = 0;

    for (int i = 0; i < index.count; i++) {
        char *path = strdup(index.entries[i].path);

        char *saveptr;
        char *token = strtok_r(path, "/", &saveptr);

        Tree *current = &root;

        while (token) {
            char *next = strtok_r(NULL, "/", &saveptr);

            if (!next) {
                TreeEntry e;
                e.mode = index.entries[i].mode;
                e.hash = index.entries[i].hash;
                snprintf(e.name, sizeof(e.name), "%s", token);

                current->entries[current->count++] = e;
            } else {
                // create/find subtree
                int found = 0;
                for (int j = 0; j < current->count; j++) {
                    if (strcmp(current->entries[j].name, token) == 0) {
                        found = 1;
                        break;
                    }
                }

                if (!found) {
                    TreeEntry dir;
                    dir.mode = 040000;
                    memset(&dir.hash, 0, sizeof(ObjectID));
                    snprintf(dir.name, sizeof(dir.name), "%s", token);
                    current->entries[current->count++] = dir;
                }
            }

            token = next;
        }

        free(path);
    }

    // serialize + store root tree
    void *raw;
    size_t len;

    tree_serialize(&root, &raw, &len);
    object_write(OBJ_TREE, raw, len, id_out);
    free(raw);

    return 0;
}
