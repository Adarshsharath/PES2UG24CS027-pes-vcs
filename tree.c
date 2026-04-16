#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ───────────── Mode Constants ─────────────
// These represent file types in octal format (similar to Git)

#define MODE_FILE      0100644   // Regular file
#define MODE_EXEC      0100755   // Executable file
#define MODE_DIR       0040000   // Directory


// ───────────── Determine File Mode ─────────────
// Returns mode based on file type and permissions
uint32_t get_file_mode(const char *path) {

    struct stat st;

    // Retrieve file metadata
    if (lstat(path, &st) != 0) {
        return 0;
    }

    // Check if path is a directory
    if (S_ISDIR(st.st_mode)) {
        return MODE_DIR;
    }

    // Check if executable bit is set
    if (st.st_mode & S_IXUSR) {
        return MODE_EXEC;
    }

    // Default: regular file
    return MODE_FILE;
}


// ───────────── Parse Tree Object ─────────────
// Converts raw binary tree data into a Tree struct
int tree_parse(const void *data, size_t len, Tree *tree_out) {

    // Initialize tree entry count
    tree_out->count = 0;

    // Set pointer to start of data
    const uint8_t *ptr = (const uint8_t *)data;

    // Define end pointer for bounds checking
    const uint8_t *end = ptr + len;


    // ───────────── Iterate through entries ─────────────
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {

        // Get reference to next tree entry
        TreeEntry *entry = &tree_out->entries[tree_out->count];


        // ───────────── Step 1: Parse mode ─────────────

        // Locate space separating mode and name
        const uint8_t *space = memchr(ptr, ' ', end - ptr);

        if (!space) {
            return -1;   // Malformed entry
        }

        // Extract mode string
        char mode_str[16] = {0};

        size_t mode_len = (size_t)(space - ptr);

        // Ensure mode string fits buffer
        if (mode_len >= sizeof(mode_str)) {
            return -1;
        }

        // Copy mode string
        memcpy(mode_str, ptr, mode_len);

        // Convert string → integer (octal)
        entry->mode = strtol(mode_str, NULL, 8);


        // Move pointer past space
        ptr = space + 1;


        // ───────────── Step 2: Parse name ─────────────

        // Find null terminator after name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);

        if (!null_byte) {
            return -1;   // Malformed entry
        }

        // Calculate name length
        size_t name_len = (size_t)(null_byte - ptr);

        // Ensure name fits buffer
        if (name_len >= sizeof(entry->name)) {
            return -1;
        }

        // Copy name
        memcpy(entry->name, ptr, name_len);

        // Add null terminator
        entry->name[name_len] = '\0';


        // Move pointer past null byte
        ptr = null_byte + 1;


        // ───────────── Step 3: Parse hash ─────────────

        // Ensure enough bytes remain for hash
        if (ptr + HASH_SIZE > end) {
            return -1;
        }

        // Copy hash (raw 32 bytes)
        memcpy(entry->hash.hash, ptr, HASH_SIZE);

        // Advance pointer
        ptr += HASH_SIZE;


        // ───────────── Step 4: Increment entry count ─────────────
        tree_out->count++;//incrementing
    }


    // ───────────── Parsing complete ─────────────
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─────────────── YOUR IMPLEMENTATION ───────────────

static int build_tree(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;

    while (i < count) {
        const char *path = entries[i].path;

        const char *rel = path;
        if (prefix && strlen(prefix) > 0) {
            if (strncmp(path, prefix, strlen(prefix)) != 0) {
                i++;
                continue;
            }
            rel = path + strlen(prefix);
        }

        char *slash = strchr(rel, '/');

        if (!slash) {
            TreeEntry *entry = &tree.entries[tree.count++];

            entry->mode = MODE_FILE;
            strcpy(entry->name, rel);
            entry->hash = entries[i].hash;   // ✅ FIXED

            i++;
        } else {
            char dirname[256];
            int len = slash - rel;
            strncpy(dirname, rel, len);
            dirname[len] = '\0';

            IndexEntry sub_entries[256];
            int sub_count = 0;

            int j = i;
            while (j < count) {
                const char *p = entries[j].path;

                if (prefix && strlen(prefix) > 0) {
                    if (strncmp(p, prefix, strlen(prefix)) != 0) {
                        j++;
                        continue;
                    }
                    p += strlen(prefix);
                }

                if (strncmp(p, dirname, len) == 0 && p[len] == '/') {
                    sub_entries[sub_count++] = entries[j];
                }
                j++;
            }

            char new_prefix[512];
            if (prefix && strlen(prefix) > 0)
                snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dirname);
            else
                snprintf(new_prefix, sizeof(new_prefix), "%s/", dirname);

            ObjectID sub_id;
            if (build_tree(sub_entries, sub_count, new_prefix, &sub_id) != 0)
                return -1;

            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = MODE_DIR;
            strcpy(entry->name, dirname);
            entry->hash = sub_id;

            i += sub_count;
        }
    }

    void *data;
    size_t len;

    if (tree_serialize(&tree, &data, &len) != 0)
        return -1;

    if (object_write(OBJ_TREE, data, len, id_out) != 0) {
        free(data);
        return -1;
    }

    free(data);
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    Index idx;

    if (index_load(&idx) != 0)
        return -1;

    return build_tree(idx.entries, idx.count, "", id_out);
}