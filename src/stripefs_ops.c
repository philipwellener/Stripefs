#include "stripefs_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Retrieve our filesystem context from the FUSE context. */
static stripefs_context_t *get_ctx(void) {
    return (stripefs_context_t *)fuse_get_context()->private_data;
}

/** Log a debug message if verbose mode is enabled. */
#define LOG_DEBUG(fmt, ...)                                                                        \
    do {                                                                                           \
        stripefs_context_t *_ctx = get_ctx();                                                      \
        if (_ctx && _ctx->verbose) {                                                               \
            fprintf(stderr, "[stripefs] " fmt "\n", ##__VA_ARGS__);                                \
        }                                                                                          \
    } while (0)

/**
 * Check if a path refers to the virtual stats file.
 */
static int is_stats_path(const char *path) {
    return strcmp(path, STATS_VIRTUAL_PATH) == 0;
}

/**
 * Check if a path or its components start with .stripe_meta (internal metadata).
 * These are hidden from directory listings.
 */
static int is_internal_path(const char *path) {
    return strstr(path, "/.stripe_meta") != NULL;
}

/**
 * getattr — Return file/directory attributes.
 *
 * For the root directory, we report a standard directory.
 * For the virtual stats file, we synthesize attributes.
 * For striped files, we compute the total size from the stripe layout.
 * For directories, we check if the directory exists on OST0.
 */
static int stripefs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    stripefs_context_t *ctx = get_ctx();
    memset(stbuf, 0, sizeof(struct stat));

    LOG_DEBUG("getattr: %s", path);

    /* Root directory */
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    /* Virtual stats file */
    if (is_stats_path(path)) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        /* Generate stats to determine size */
        char buf[8192];
        int len = stats_to_json(ctx->stats, buf, sizeof(buf));
        stbuf->st_size = len > 0 ? len : 0;
        return 0;
    }

    /* Hide internal metadata paths */
    if (is_internal_path(path)) {
        return -ENOENT;
    }

    /* Check if it's a directory (on OST0) */
    char dir_path[4096];
    if (stripe_dir_path(ctx->stripe_config, path, 0, dir_path, sizeof(dir_path)) == 0) {
        struct stat ost_stat;
        if (stat(dir_path, &ost_stat) == 0 && S_ISDIR(ost_stat.st_mode)) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            stbuf->st_uid = ost_stat.st_uid;
            stbuf->st_gid = ost_stat.st_gid;
            stbuf->st_atime = ost_stat.st_atime;
            stbuf->st_mtime = ost_stat.st_mtime;
            return 0;
        }
    }

    /* Check if it's a striped file (has layout metadata) */
    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, path);
    if (!layout) {
        return -ENOENT;
    }

    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_size = (off_t)layout->total_size;

    /* Get timestamps from the first stripe chunk */
    char chunk_path[4096];
    if (stripe_chunk_path(ctx->stripe_config, path, layout->start_ost, 0, chunk_path,
                          sizeof(chunk_path)) == 0) {
        struct stat chunk_stat;
        if (stat(chunk_path, &chunk_stat) == 0) {
            stbuf->st_uid = chunk_stat.st_uid;
            stbuf->st_gid = chunk_stat.st_gid;
            stbuf->st_atime = chunk_stat.st_atime;
            stbuf->st_mtime = chunk_stat.st_mtime;
        }
    }

    free(layout);
    return 0;
}

/**
 * readdir — List directory contents.
 *
 * Merges the directory listing from OST0 (where all directory structures are
 * mirrored) and deduplicates stripe chunk files into their logical file names.
 * Hides internal .stripe_meta directories.
 */
static int stripefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("readdir: %s", path);

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* Add virtual stats file at root */
    if (strcmp(path, "/") == 0) {
        filler(buf, ".stripefs_stats", NULL, 0, 0);
    }

    /* List directory on OST0 to find subdirectories */
    char dir_path[4096];
    if (stripe_dir_path(ctx->stripe_config, path, 0, dir_path, sizeof(dir_path)) != 0) {
        return -EIO;
    }

    DIR *dp = opendir(dir_path);
    if (!dp) {
        /* Root with no subdirs is OK */
        if (strcmp(path, "/") == 0) {
            /* Still list files from metadata */
        } else {
            return -ENOENT;
        }
    }

    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (strcmp(de->d_name, ".stripe_meta") == 0) continue;

            /* Skip stripe chunk files (they have .sN suffix) */
            char *dot_s = strstr(de->d_name, ".s");
            if (dot_s) {
                /* Check if everything after .s is digits (stripe chunk) */
                char *after = dot_s + 2;
                int is_chunk = 1;
                if (*after == '\0') is_chunk = 0;
                while (*after) {
                    if (*after < '0' || *after > '9') {
                        is_chunk = 0;
                        break;
                    }
                    after++;
                }
                if (is_chunk) continue; /* Skip stripe chunks */
            }

            /* Check if it's a directory */
            char full_path[4096 + 256 + 2];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                filler(buf, de->d_name, NULL, 0, 0);
            }
        }
        closedir(dp);
    }

    /* List files from metadata directory */
    char meta_dir[4096];
    snprintf(meta_dir, sizeof(meta_dir), "%s%s", ctx->stripe_config->meta_dir, path);

    DIR *mdp = opendir(meta_dir);
    if (mdp) {
        struct dirent *de;
        while ((de = readdir(mdp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

            /* Strip .layout suffix to get original filename */
            char *suffix = strstr(de->d_name, ".layout");
            if (suffix && *(suffix + 7) == '\0') {
                char name[256];
                size_t name_len = (size_t)(suffix - de->d_name);
                if (name_len < sizeof(name)) {
                    memcpy(name, de->d_name, name_len);
                    name[name_len] = '\0';
                    filler(buf, name, NULL, 0, 0);
                }
            }
        }
        closedir(mdp);
    }

    return 0;
}

/**
 * open — Open a file for reading or writing.
 *
 * For existing files, verify the stripe layout exists.
 * For creation, this is handled by create().
 */
static int stripefs_open(const char *path, struct fuse_file_info *fi) {
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("open: %s flags=0x%x", path, fi->flags);

    if (is_stats_path(path)) return 0;

    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, path);
    if (!layout) {
        return -ENOENT;
    }

    /* Handle O_TRUNC — truncate file to zero on open */
    if (fi->flags & O_TRUNC) {
        int total_stripes = 0;
        if (layout->total_size > 0) {
            total_stripes =
                (int)((layout->total_size + layout->stripe_size - 1) / layout->stripe_size);
        }
        char chunk_path[4096];
        for (int s = 0; s < total_stripes; s++) {
            int ost = (layout->start_ost + s) % ctx->stripe_config->ost_count;
            if (stripe_chunk_path(ctx->stripe_config, path, ost, s, chunk_path,
                                  sizeof(chunk_path)) == 0) {
                unlink(chunk_path);
            }
        }
        layout->total_size = 0;
        stripe_save_layout(ctx->stripe_config, path, layout);
        cache_invalidate(ctx->cache, path);
    }

    free(layout);
    return 0;
}

/**
 * read — Read data from a striped file.
 *
 * This is the core operation. It:
 * 1. Checks the cache for the requested data
 * 2. On miss, maps the read to stripe extents
 * 3. Reads each stripe chunk from the correct OST
 * 4. Reassembles the data into a contiguous buffer
 * 5. Caches the result for future reads
 */
static int stripefs_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi) {
    (void)fi;
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("read: %s offset=%lld size=%zu", path, (long long)offset, size);

    stats_record_read_op(ctx->stats);

    /* Handle virtual stats file */
    if (is_stats_path(path)) {
        char stats_buf[8192];
        int len = stats_to_json(ctx->stats, stats_buf, sizeof(stats_buf));
        if (len < 0) return -EIO;

        if (offset >= len) return 0;
        size_t avail = (size_t)(len - offset);
        size_t to_copy = size < avail ? size : avail;
        memcpy(buf, stats_buf + offset, to_copy);
        return (int)to_copy;
    }

    /* Load stripe layout */
    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, path);
    if (!layout) return -ENOENT;

    /* Clamp to file size */
    if (offset >= (off_t)layout->total_size) {
        free(layout);
        return 0;
    }
    size_t avail = layout->total_size - (size_t)offset;
    if (size > avail) size = avail;

    /* Check cache — keyed by path (caches entire file content).
     * We allocate a temporary buffer for the cached data since cache_get()
     * copies data under the lock to avoid use-after-free. */
    size_t cached_total = 0;
    ssize_t cache_result = cache_get(ctx->cache, path, NULL, 0, &cached_total);
    if (cache_result >= 0 && cached_total >= (size_t)offset + size) {
        /* Full hit — allocate temp buffer, copy the region we need */
        char *cached_buf = malloc(cached_total);
        if (cached_buf) {
            ssize_t got = cache_get(ctx->cache, path, cached_buf, cached_total, NULL);
            if (got >= 0 && (size_t)got >= (size_t)offset + size) {
                memcpy(buf, cached_buf + offset, size);
                free(cached_buf);
                stats_record_cache_hit(ctx->stats, size);
                free(layout);
                LOG_DEBUG("read: cache hit for %s", path);
                return (int)size;
            }
            free(cached_buf);
        }
    }
    stats_record_cache_miss(ctx->stats);

    /* Cache miss — read from stripe chunks */
    stripe_extent_t extents[MAX_EXTENTS];
    int extent_count = 0;

    if (stripe_map_read(ctx->stripe_config, layout, offset, size, extents, &extent_count) != 0) {
        free(layout);
        return -EIO;
    }

    /* Read each extent from the appropriate OST */
    size_t total_read = 0;
    for (int i = 0; i < extent_count; i++) {
        stripe_extent_t *ext = &extents[i];

        char chunk_path[4096];
        if (stripe_chunk_path(ctx->stripe_config, path, ext->ost_index, ext->stripe_index,
                              chunk_path, sizeof(chunk_path)) != 0) {
            free(layout);
            return -EIO;
        }

        int fd = open(chunk_path, O_RDONLY);
        if (fd < 0) {
            /* Missing chunk — fill with zeros (sparse file behavior) */
            size_t buf_offset = (size_t)(ext->file_offset - offset);
            memset(buf + buf_offset, 0, ext->length);
            total_read += ext->length;
            continue;
        }

        size_t buf_offset = (size_t)(ext->file_offset - offset);
        ssize_t n = pread(fd, buf + buf_offset, ext->length, ext->local_offset);
        close(fd);

        if (n < 0) {
            free(layout);
            return -EIO;
        }

        total_read += (size_t)n;
        stats_record_ost_read(ctx->stats, ext->ost_index, (size_t)n);
    }

    /* Cache the entire file for future reads (if it fits) */
    if (layout->total_size <= ctx->cache->max_size) {
        /* Read the entire file into a buffer for caching */
        char *file_buf = malloc(layout->total_size);
        if (file_buf) {
            stripe_extent_t all_extents[MAX_EXTENTS];
            int all_count = 0;
            stripe_map_read(ctx->stripe_config, layout, 0, layout->total_size, all_extents,
                            &all_count);

            int cache_ok = 1;
            for (int i = 0; i < all_count; i++) {
                stripe_extent_t *ext = &all_extents[i];
                char chunk_path[4096];
                if (stripe_chunk_path(ctx->stripe_config, path, ext->ost_index, ext->stripe_index,
                                      chunk_path, sizeof(chunk_path)) != 0) {
                    cache_ok = 0;
                    break;
                }

                int fd = open(chunk_path, O_RDONLY);
                if (fd < 0) {
                    memset(file_buf + ext->file_offset, 0, ext->length);
                    continue;
                }

                ssize_t n = pread(fd, file_buf + ext->file_offset, ext->length, ext->local_offset);
                close(fd);
                if (n < 0) {
                    cache_ok = 0;
                    break;
                }
            }

            if (cache_ok) {
                cache_put(ctx->cache, path, file_buf, layout->total_size);
            }
            free(file_buf);
        }
    }

    free(layout);
    return (int)total_read;
}

/**
 * write — Write data to a striped file.
 *
 * Splits incoming data into stripe-sized chunks and distributes them
 * round-robin across OST directories. Invalidates cache for the file.
 */
static int stripefs_write(const char *path, const char *buf, size_t size, off_t offset,
                          struct fuse_file_info *fi) {
    (void)fi;
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("write: %s offset=%lld size=%zu", path, (long long)offset, size);

    if (is_stats_path(path)) return -EACCES;

    stats_record_write_op(ctx->stats);

    /* Load or create stripe layout */
    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, path);
    int new_file = 0;
    if (!layout) {
        layout = calloc(1, sizeof(stripe_layout_t));
        if (!layout) return -ENOMEM;
        layout->ost_count = ctx->stripe_config->ost_count;
        layout->stripe_size = ctx->stripe_config->stripe_size;
        layout->total_size = 0;
        layout->start_ost = 0;
        new_file = 1;
    }

    /* Map the write to stripe extents */
    stripe_extent_t extents[MAX_EXTENTS];
    int extent_count = 0;

    if (stripe_map_write(ctx->stripe_config, layout, offset, size, extents, &extent_count) != 0) {
        free(layout);
        return -EIO;
    }

    /* Ensure parent directories exist on all OSTs for the file */
    if (new_file) {
        /* Create parent directories on all OSTs */
        char *path_copy = strdup(path);
        if (path_copy) {
            char *parent = path_copy;
            /* Find last slash */
            char *last_slash = strrchr(parent, '/');
            if (last_slash && last_slash != parent) {
                *last_slash = '\0';
                stripe_mkdir(ctx->stripe_config, parent, 0755);
            }
            free(path_copy);
        }
    }

    /* Write each extent to the appropriate OST */
    size_t total_written = 0;
    for (int i = 0; i < extent_count; i++) {
        stripe_extent_t *ext = &extents[i];

        char chunk_path[4096];
        if (stripe_chunk_path(ctx->stripe_config, path, ext->ost_index, ext->stripe_index,
                              chunk_path, sizeof(chunk_path)) != 0) {
            free(layout);
            return -EIO;
        }

        int fd = open(chunk_path, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            free(layout);
            return -errno;
        }

        size_t buf_offset = (size_t)(ext->file_offset - offset);
        ssize_t n = pwrite(fd, buf + buf_offset, ext->length, ext->local_offset);
        close(fd);

        if (n < 0) {
            free(layout);
            return -errno;
        }

        total_written += (size_t)n;
        stats_record_ost_write(ctx->stats, ext->ost_index, (size_t)n);
    }

    /* Update total file size */
    size_t new_end = (size_t)offset + total_written;
    if (new_end > layout->total_size) {
        layout->total_size = new_end;
    }

    /* Save updated layout */
    stripe_save_layout(ctx->stripe_config, path, layout);

    /* Invalidate cache for this file */
    cache_invalidate(ctx->cache, path);

    free(layout);
    return (int)total_written;
}

/**
 * create — Create a new file and open it.
 *
 * Creates the stripe layout metadata and prepares OST directories.
 */
static int stripefs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    (void)mode;
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("create: %s", path);

    if (is_stats_path(path)) return -EACCES;

    /* Create stripe layout for the new file */
    stripe_layout_t layout = {.ost_count = ctx->stripe_config->ost_count,
                              .stripe_size = ctx->stripe_config->stripe_size,
                              .total_size = 0,
                              .start_ost = 0};

    /* Ensure parent directories exist on all OSTs */
    char *path_copy = strdup(path);
    if (path_copy) {
        char *last_slash = strrchr(path_copy, '/');
        if (last_slash && last_slash != path_copy) {
            *last_slash = '\0';
            stripe_mkdir(ctx->stripe_config, path_copy, 0755);
        }
        free(path_copy);
    }

    /* Save layout metadata */
    if (stripe_save_layout(ctx->stripe_config, path, &layout) != 0) {
        return -EIO;
    }

    return 0;
}

/**
 * unlink — Delete a file.
 *
 * Removes all stripe chunks from all OSTs and deletes the layout metadata.
 */
static int stripefs_unlink(const char *path) {
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("unlink: %s", path);

    if (is_stats_path(path)) return -EACCES;

    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, path);
    if (!layout) return -ENOENT;

    stripe_delete_file(ctx->stripe_config, path, layout);
    cache_invalidate(ctx->cache, path);

    free(layout);
    return 0;
}

/**
 * mkdir — Create a directory on all OSTs.
 */
static int stripefs_mkdir(const char *path, mode_t mode) {
    stripefs_context_t *ctx = get_ctx();
    LOG_DEBUG("mkdir: %s", path);
    return stripe_mkdir(ctx->stripe_config, path, mode) == 0 ? 0 : -EIO;
}

/**
 * rmdir — Remove a directory from all OSTs.
 */
static int stripefs_rmdir(const char *path) {
    stripefs_context_t *ctx = get_ctx();
    LOG_DEBUG("rmdir: %s", path);
    return stripe_rmdir(ctx->stripe_config, path) == 0 ? 0 : -EIO;
}

/**
 * truncate — Truncate a file to a specified length.
 *
 * Adjusts stripe chunks accordingly and invalidates cache.
 */
static int stripefs_truncate(const char *path, off_t length, struct fuse_file_info *fi) {
    (void)fi;
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("truncate: %s length=%lld", path, (long long)length);

    if (is_stats_path(path)) return -EACCES;
    if (length < 0) return -EINVAL;

    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, path);
    if (!layout) return -ENOENT;

    if (length == 0) {
        /* Truncate to zero — delete only stripe chunks, not metadata.
         * We avoid stripe_delete_file() here because it also removes the
         * metadata file, which would create a window where the file appears
         * non-existent to concurrent getattr/open calls. */
        int total_stripes = 0;
        if (layout->total_size > 0) {
            total_stripes =
                (int)((layout->total_size + layout->stripe_size - 1) / layout->stripe_size);
        }
        char chunk_path[4096];
        for (int s = 0; s < total_stripes; s++) {
            int ost = (layout->start_ost + s) % ctx->stripe_config->ost_count;
            if (stripe_chunk_path(ctx->stripe_config, path, ost, s, chunk_path,
                                  sizeof(chunk_path)) == 0) {
                unlink(chunk_path);
            }
        }
        layout->total_size = 0;
        stripe_save_layout(ctx->stripe_config, path, layout);
    } else {
        /* Calculate which stripe the new end falls in */
        int last_stripe = (int)((size_t)length / layout->stripe_size);
        size_t pos_in_last = (size_t)length % layout->stripe_size;

        /* Truncate the last stripe chunk */
        int last_ost = (layout->start_ost + last_stripe) % ctx->stripe_config->ost_count;
        char chunk_path[4096];
        if (stripe_chunk_path(ctx->stripe_config, path, last_ost, last_stripe, chunk_path,
                              sizeof(chunk_path)) == 0) {
            if (truncate(chunk_path, (off_t)pos_in_last) != 0) {
                /* Best effort — truncate may fail if chunk doesn't exist yet */
            }
        }

        /* Delete any stripe chunks beyond the new size */
        int old_total_stripes = 0;
        if (layout->total_size > 0) {
            old_total_stripes =
                (int)((layout->total_size + layout->stripe_size - 1) / layout->stripe_size);
        }

        for (int s = last_stripe + 1; s < old_total_stripes; s++) {
            int ost = (layout->start_ost + s) % ctx->stripe_config->ost_count;
            if (stripe_chunk_path(ctx->stripe_config, path, ost, s, chunk_path,
                                  sizeof(chunk_path)) == 0) {
                unlink(chunk_path);
            }
        }

        layout->total_size = (size_t)length;
        stripe_save_layout(ctx->stripe_config, path, layout);
    }

    cache_invalidate(ctx->cache, path);
    free(layout);
    return 0;
}

/**
 * utimens — Update file timestamps.
 *
 * Updates timestamps on the first stripe chunk (OST0's copy).
 */
static int stripefs_utimens(const char *path, const struct timespec tv[2],
                            struct fuse_file_info *fi) {
    (void)fi;
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("utimens: %s", path);

    if (is_stats_path(path)) return 0;

    /* Update timestamps on first stripe chunk */
    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, path);
    if (!layout) {
        /* Might be a directory */
        char dir_path[4096];
        if (stripe_dir_path(ctx->stripe_config, path, 0, dir_path, sizeof(dir_path)) == 0) {
            return utimensat(AT_FDCWD, dir_path, tv, 0) == 0 ? 0 : -errno;
        }
        return -ENOENT;
    }

    char chunk_path[4096];
    if (stripe_chunk_path(ctx->stripe_config, path, layout->start_ost, 0, chunk_path,
                          sizeof(chunk_path)) == 0) {
        utimensat(AT_FDCWD, chunk_path, tv, 0);
    }

    free(layout);
    return 0;
}

/**
 * release — Called when a file is closed.
 */
static int stripefs_release(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    LOG_DEBUG("release: %s", path);
    return 0;
}

/**
 * chmod — Change file permissions (no-op for simplicity, report success).
 */
static int stripefs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)path;
    (void)mode;
    (void)fi;
    return 0;
}

/**
 * chown — Change file ownership (no-op for simplicity, report success).
 */
static int stripefs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)path;
    (void)uid;
    (void)gid;
    (void)fi;
    return 0;
}

/**
 * rename — Rename/move a file.
 *
 * Updates stripe layout metadata and renames all stripe chunks across OSTs.
 */
static int stripefs_rename(const char *from, const char *to, unsigned int flags) {
    (void)flags;
    stripefs_context_t *ctx = get_ctx();

    LOG_DEBUG("rename: %s -> %s", from, to);

    /* Check if source is a directory */
    char dir_path[4096];
    if (stripe_dir_path(ctx->stripe_config, from, 0, dir_path, sizeof(dir_path)) == 0) {
        struct stat st;
        if (stat(dir_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Rename directory on all OSTs */
            for (int i = 0; i < ctx->stripe_config->ost_count; i++) {
                char from_path[4096], to_path[4096];
                stripe_dir_path(ctx->stripe_config, from, i, from_path, sizeof(from_path));
                stripe_dir_path(ctx->stripe_config, to, i, to_path, sizeof(to_path));
                rename(from_path, to_path);
            }
            return 0;
        }
    }

    stripe_layout_t *layout = stripe_load_layout(ctx->stripe_config, from);
    if (!layout) return -ENOENT;

    /* Ensure destination parent dirs exist */
    char *to_copy = strdup(to);
    if (to_copy) {
        char *last_slash = strrchr(to_copy, '/');
        if (last_slash && last_slash != to_copy) {
            *last_slash = '\0';
            stripe_mkdir(ctx->stripe_config, to_copy, 0755);
        }
        free(to_copy);
    }

    /* Rename all stripe chunks */
    int total_stripes = 0;
    if (layout->total_size > 0) {
        total_stripes = (int)((layout->total_size + layout->stripe_size - 1) / layout->stripe_size);
    }

    for (int s = 0; s < total_stripes; s++) {
        int ost = (layout->start_ost + s) % ctx->stripe_config->ost_count;
        char old_chunk[4096], new_chunk[4096];
        stripe_chunk_path(ctx->stripe_config, from, ost, s, old_chunk, sizeof(old_chunk));
        stripe_chunk_path(ctx->stripe_config, to, ost, s, new_chunk, sizeof(new_chunk));
        rename(old_chunk, new_chunk);
    }

    /* Save layout under new path and delete old metadata */
    stripe_ensure_meta_dir(ctx->stripe_config, to);
    stripe_save_layout(ctx->stripe_config, to, layout);

    /* Remove old metadata */
    char old_meta[4096];
    stripe_meta_path(ctx->stripe_config, from, old_meta, sizeof(old_meta));
    unlink(old_meta);

    /* Invalidate cache for both old and new paths */
    cache_invalidate(ctx->cache, from);
    cache_invalidate(ctx->cache, to);

    free(layout);
    return 0;
}

/** Static FUSE operations structure. */
static struct fuse_operations stripefs_operations = {
    .getattr = stripefs_getattr,
    .readdir = stripefs_readdir,
    .open = stripefs_open,
    .read = stripefs_read,
    .write = stripefs_write,
    .create = stripefs_create,
    .unlink = stripefs_unlink,
    .mkdir = stripefs_mkdir,
    .rmdir = stripefs_rmdir,
    .truncate = stripefs_truncate,
    .utimens = stripefs_utimens,
    .release = stripefs_release,
    .chmod = stripefs_chmod,
    .chown = stripefs_chown,
    .rename = stripefs_rename,
};

struct fuse_operations *stripefs_get_ops(void) {
    return &stripefs_operations;
}
