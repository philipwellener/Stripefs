#include "stripe.h"

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

stripe_config_t *stripe_init(int ost_count, size_t stripe_size, const char **ost_paths) {
    if (ost_count <= 0 || !ost_paths || stripe_size == 0) {
        return NULL;
    }

    stripe_config_t *config = calloc(1, sizeof(stripe_config_t));
    if (!config) return NULL;

    config->ost_count = ost_count;
    config->stripe_size = stripe_size;

    config->ost_paths = calloc((size_t)ost_count, sizeof(char *));
    if (!config->ost_paths) {
        free(config);
        return NULL;
    }

    for (int i = 0; i < ost_count; i++) {
        config->ost_paths[i] = strdup(ost_paths[i]);
        if (!config->ost_paths[i]) {
            for (int j = 0; j < i; j++) free(config->ost_paths[j]);
            free(config->ost_paths);
            free(config);
            return NULL;
        }
    }

    /* Metadata directory lives inside OST0 */
    size_t meta_len = strlen(ost_paths[0]) + strlen("/.stripe_meta") + 1;
    config->meta_dir = malloc(meta_len);
    if (!config->meta_dir) {
        stripe_destroy(config);
        return NULL;
    }
    snprintf(config->meta_dir, meta_len, "%s/.stripe_meta", ost_paths[0]);

    /* Create the metadata root directory */
    mkdir(config->meta_dir, 0755);

    return config;
}

void stripe_destroy(stripe_config_t *config) {
    if (!config) return;

    if (config->ost_paths) {
        for (int i = 0; i < config->ost_count; i++) {
            free(config->ost_paths[i]);
        }
        free(config->ost_paths);
    }

    free(config->meta_dir);
    free(config);
}

/**
 * Core stripe mapping logic used by both read and write.
 *
 * Maps a contiguous logical region [offset, offset+length) of a striped file
 * into a list of extents — each describing a chunk of data on a specific OST.
 *
 * The mapping works by:
 * 1. Finding which stripe chunk the offset falls into (stripe_idx = offset / stripe_size)
 * 2. Computing the position within that chunk (pos_in_stripe = offset % stripe_size)
 * 3. Determining which OST holds that chunk (ost = (start_ost + stripe_idx) % ost_count)
 * 4. Reading as much as possible from that chunk, then advancing to the next stripe
 */
static int stripe_map_region(stripe_config_t *config, stripe_layout_t *layout, off_t offset,
                             size_t length, stripe_extent_t *extents, int *extent_count) {
    if (!config || !layout || !extents || !extent_count) {
        return -1;
    }

    *extent_count = 0;
    size_t remaining = length;
    off_t pos = offset;

    while (remaining > 0 && *extent_count < MAX_EXTENTS) {
        /* Which stripe chunk does this offset fall in? */
        int stripe_idx = (int)(pos / (off_t)config->stripe_size);

        /* Position within the stripe chunk */
        off_t pos_in_stripe = pos % (off_t)config->stripe_size;

        /* How many bytes can we read/write from this stripe chunk? */
        size_t chunk_remaining = config->stripe_size - (size_t)pos_in_stripe;
        size_t extent_len = remaining < chunk_remaining ? remaining : chunk_remaining;

        /* Which OST holds this stripe chunk? (round-robin with start offset) */
        int ost = (layout->start_ost + stripe_idx) % config->ost_count;

        stripe_extent_t *ext = &extents[*extent_count];
        ext->ost_index = ost;
        ext->stripe_index = stripe_idx;
        ext->local_offset = pos_in_stripe;
        ext->length = extent_len;
        ext->file_offset = pos;

        (*extent_count)++;
        pos += (off_t)extent_len;
        remaining -= extent_len;
    }

    return 0;
}

int stripe_map_read(stripe_config_t *config, stripe_layout_t *layout, off_t offset, size_t length,
                    stripe_extent_t *extents, int *extent_count) {
    if (!config || !layout || !extents || !extent_count) {
        return -1;
    }

    /* Clamp read to file size */
    if (offset >= (off_t)layout->total_size) {
        *extent_count = 0;
        return 0;
    }

    size_t avail = layout->total_size - (size_t)offset;
    if (length > avail) {
        length = avail;
    }

    return stripe_map_region(config, layout, offset, length, extents, extent_count);
}

int stripe_map_write(stripe_config_t *config, stripe_layout_t *layout, off_t offset, size_t length,
                     stripe_extent_t *extents, int *extent_count) {
    return stripe_map_region(config, layout, offset, length, extents, extent_count);
}

int stripe_chunk_path(stripe_config_t *config, const char *rel_path, int ost_index,
                      int stripe_index, char *buf, size_t buf_size) {
    if (!config || !rel_path || !buf || ost_index < 0 || ost_index >= config->ost_count) {
        return -1;
    }

    int n = snprintf(buf, buf_size, "%s%s.s%d", config->ost_paths[ost_index], rel_path,
                     stripe_index);
    if (n < 0 || (size_t)n >= buf_size) {
        return -1;
    }
    return 0;
}

int stripe_meta_path(stripe_config_t *config, const char *rel_path, char *buf, size_t buf_size) {
    if (!config || !rel_path || !buf) {
        return -1;
    }

    int n = snprintf(buf, buf_size, "%s%s.layout", config->meta_dir, rel_path);
    if (n < 0 || (size_t)n >= buf_size) {
        return -1;
    }
    return 0;
}

int stripe_dir_path(stripe_config_t *config, const char *rel_path, int ost_index, char *buf,
                    size_t buf_size) {
    if (!config || !rel_path || !buf || ost_index < 0 || ost_index >= config->ost_count) {
        return -1;
    }

    int n = snprintf(buf, buf_size, "%s%s", config->ost_paths[ost_index], rel_path);
    if (n < 0 || (size_t)n >= buf_size) {
        return -1;
    }
    return 0;
}

/**
 * Recursively create all directories in path (like mkdir -p).
 */
static int mkdirs(const char *path, mode_t mode) {
    char *tmp = strdup(path);
    if (!tmp) return -1;

    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

int stripe_ensure_meta_dir(stripe_config_t *config, const char *rel_path) {
    char meta_path[4096];
    if (stripe_meta_path(config, rel_path, meta_path, sizeof(meta_path)) != 0) {
        return -1;
    }

    /* Get the parent directory of the metadata file */
    char *tmp = strdup(meta_path);
    if (!tmp) return -1;
    char *dir = dirname(tmp);
    int rc = mkdirs(dir, 0755);
    free(tmp);
    return rc;
}

int stripe_save_layout(stripe_config_t *config, const char *rel_path, stripe_layout_t *layout) {
    if (stripe_ensure_meta_dir(config, rel_path) != 0) {
        return -1;
    }

    char meta_path[4096];
    if (stripe_meta_path(config, rel_path, meta_path, sizeof(meta_path)) != 0) {
        return -1;
    }

    FILE *f = fopen(meta_path, "w");
    if (!f) return -1;

    fprintf(f, "%d\n%zu\n%zu\n%d\n", layout->ost_count, layout->stripe_size, layout->total_size,
            layout->start_ost);

    fclose(f);
    return 0;
}

stripe_layout_t *stripe_load_layout(stripe_config_t *config, const char *rel_path) {
    char meta_path[4096];
    if (stripe_meta_path(config, rel_path, meta_path, sizeof(meta_path)) != 0) {
        return NULL;
    }

    FILE *f = fopen(meta_path, "r");
    if (!f) return NULL;

    stripe_layout_t *layout = calloc(1, sizeof(stripe_layout_t));
    if (!layout) {
        fclose(f);
        return NULL;
    }

    if (fscanf(f, "%d\n%zu\n%zu\n%d\n", &layout->ost_count, &layout->stripe_size,
               &layout->total_size, &layout->start_ost) != 4) {
        free(layout);
        fclose(f);
        return NULL;
    }

    fclose(f);

    /* Validate loaded fields to guard against corrupt metadata */
    if (layout->ost_count <= 0 || layout->stripe_size == 0 || layout->start_ost < 0 ||
        layout->start_ost >= layout->ost_count) {
        free(layout);
        return NULL;
    }

    return layout;
}

int stripe_delete_file(stripe_config_t *config, const char *rel_path, stripe_layout_t *layout) {
    if (!config || !rel_path || !layout) return -1;

    /* Calculate total number of stripe chunks */
    int total_stripes = 0;
    if (layout->total_size > 0) {
        total_stripes = (int)((layout->total_size + layout->stripe_size - 1) / layout->stripe_size);
    }

    /* Delete all stripe chunk files */
    char chunk_path[4096];
    for (int i = 0; i < total_stripes; i++) {
        int ost = (layout->start_ost + i) % config->ost_count;
        if (stripe_chunk_path(config, rel_path, ost, i, chunk_path, sizeof(chunk_path)) == 0) {
            unlink(chunk_path);
        }
    }

    /* Delete the metadata file */
    char meta_path[4096];
    if (stripe_meta_path(config, rel_path, meta_path, sizeof(meta_path)) == 0) {
        unlink(meta_path);
    }

    return 0;
}

ssize_t stripe_compute_size(stripe_config_t *config, const char *rel_path,
                            stripe_layout_t *layout) {
    if (!config || !rel_path || !layout) return -1;

    /* Find the highest stripe index that exists */
    int max_stripe = -1;
    char chunk_path[4096];

    for (int s = 0; s < 10000; s++) { /* Safety limit */
        int ost = (layout->start_ost + s) % config->ost_count;
        if (stripe_chunk_path(config, rel_path, ost, s, chunk_path, sizeof(chunk_path)) != 0) {
            break;
        }

        struct stat st;
        if (stat(chunk_path, &st) == 0) {
            max_stripe = s;
        } else {
            break; /* No more stripes */
        }
    }

    if (max_stripe < 0) return 0; /* No stripes = empty file */

    /* Total size = (max_stripe * stripe_size) + size_of_last_stripe */
    int last_ost = (layout->start_ost + max_stripe) % config->ost_count;
    if (stripe_chunk_path(config, rel_path, last_ost, max_stripe, chunk_path,
                          sizeof(chunk_path)) != 0) {
        return -1;
    }

    struct stat st;
    if (stat(chunk_path, &st) != 0) return -1;

    return (ssize_t)((size_t)max_stripe * layout->stripe_size + (size_t)st.st_size);
}

int stripe_mkdir(stripe_config_t *config, const char *rel_path, mode_t mode) {
    if (!config || !rel_path) return -1;

    /* Create the directory on all OSTs */
    char dir_path[4096];
    for (int i = 0; i < config->ost_count; i++) {
        if (stripe_dir_path(config, rel_path, i, dir_path, sizeof(dir_path)) != 0) {
            return -1;
        }
        if (mkdirs(dir_path, mode) != 0) {
            return -1;
        }
    }

    /* Also create the corresponding metadata directory */
    char meta_subdir[4096];
    snprintf(meta_subdir, sizeof(meta_subdir), "%s%s", config->meta_dir, rel_path);
    if (mkdirs(meta_subdir, mode) != 0) {
        return -1;
    }

    return 0;
}

int stripe_rmdir(stripe_config_t *config, const char *rel_path) {
    if (!config || !rel_path) return -1;

    char dir_path[4096];
    for (int i = 0; i < config->ost_count; i++) {
        if (stripe_dir_path(config, rel_path, i, dir_path, sizeof(dir_path)) != 0) {
            return -1;
        }
        rmdir(dir_path); /* May fail if not empty, that's OK */
    }

    /* Remove metadata directory */
    char meta_subdir[4096];
    snprintf(meta_subdir, sizeof(meta_subdir), "%s%s", config->meta_dir, rel_path);
    rmdir(meta_subdir);

    return 0;
}
