#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
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

// ─── IMPLEMENTATION ─────────────────────────────────────────────────────────

// Convert enum to string
const char* type_to_string(ObjectType type) {
    switch (type) {
        case OBJ_BLOB: return "blob";
        case OBJ_TREE: return "tree";
        case OBJ_COMMIT: return "commit";
        default: return "";
    }
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Create header
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_to_string(type), len);

    // total size = header + '\0' + data
    size_t total_len = header_len + 1 + len;
    char *full = malloc(total_len);
    if (!full) return -1;

    memcpy(full, header, header_len);
    full[header_len] = '\0';
    memcpy(full + header_len + 1, data, len);

    // 2. Compute hash
    compute_hash(full, total_len, id_out);

    // 3. Deduplication check
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    // 4. Create directory
    char dir_path[512];
    char file_path[512];
    object_path(id_out, file_path, sizeof(file_path));

    snprintf(dir_path, sizeof(dir_path), "%s/%.2s", OBJECTS_DIR, file_path + strlen(OBJECTS_DIR) + 1);

    mkdir(dir_path, 0755);

    // 5. Temp file path
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", file_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    // 6. Write
    if (write(fd, full, total_len) != (ssize_t)total_len) {
        close(fd);
        free(full);
        return -1;
    }

    // 7. fsync file
    fsync(fd);
    close(fd);

    // 8. rename (atomic)
    if (rename(tmp_path, file_path) != 0) {
        free(full);
        return -1;
    }

    free(full);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    // get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    if (fread(buffer, 1, size, f) != (size_t)size) {
        free(buffer);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 4. Verify integrity
    ObjectID check;
    compute_hash(buffer, size, &check);
    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    // 3. Parse header
    char *null_pos = memchr(buffer, '\0', size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    // extract type
    if (strncmp(buffer, "blob", 4) == 0)
        *type_out = OBJ_BLOB;
    else if (strncmp(buffer, "tree", 4) == 0)
        *type_out = OBJ_TREE;
    else if (strncmp(buffer, "commit", 6) == 0)
        *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    // extract size
    size_t data_len = size - (null_pos - buffer) - 1;

    // copy data
    *data_out = malloc(data_len);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, null_pos + 1, data_len);
    *len_out = data_len;

    free(buffer);
    return 0;
}
// object_write: stores blob/tree/commit with SHA-256 hash
