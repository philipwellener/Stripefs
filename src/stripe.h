#ifndef STRIPEFS_STRIPE_H
#define STRIPEFS_STRIPE_H

#include <stddef.h>
#include <sys/types.h>

/** Maximum number of stripe extents a single read/write can span. */
#define MAX_EXTENTS 256

/**
 * Stripe configuration — defines how files are distributed across OSTs.
 * Created once at mount time and shared across all FUSE operations.
 */
typedef struct stripe_config {
    int ost_count;        /* Number of backing directories (Object Storage Targets) */
    size_t stripe_size;   /* Bytes per stripe chunk (default: 1MB like Lustre) */
    char **ost_paths;     /* Array of backing directory paths */
    char *meta_dir;       /* Path for stripe layout metadata (inside OST0) */
} stripe_config_t;

/**
 * Per-file stripe layout, stored as metadata on disk.
 * Mirrors Lustre's LOV (Logical Object Volume) metadata.
 */
typedef struct stripe_layout {
    int ost_count;        /* Number of OSTs this file spans */
    size_t stripe_size;   /* Stripe chunk size for this file */
    size_t total_size;    /* Total logical file size */
    int start_ost;        /* Starting OST index (for round-robin offset) */
} stripe_layout_t;

/**
 * A single extent describing a contiguous region within one OST's stripe chunk.
 * Used to map a logical file offset+length to physical stripe locations.
 */
typedef struct stripe_extent {
    int ost_index;        /* Which OST this extent lives on */
    int stripe_index;     /* Which stripe chunk within the file */
    off_t local_offset;   /* Offset within the stripe chunk file */
    size_t length;        /* Number of bytes in this extent */
    off_t file_offset;    /* Offset in the original logical file */
} stripe_extent_t;

/**
 * Initialize stripe configuration.
 * @param ost_count Number of OSTs.
 * @param stripe_size Bytes per stripe chunk.
 * @param ost_paths Array of ost_count directory paths (copied internally).
 * @return Heap-allocated config, or NULL on failure.
 */
stripe_config_t *stripe_init(int ost_count, size_t stripe_size, const char **ost_paths);

/** Free a stripe configuration. */
void stripe_destroy(stripe_config_t *config);

/**
 * Map a logical read region to a list of stripe extents.
 *
 * Given a file's stripe layout, a logical offset, and a read length, produces
 * an array of extents describing which OSTs to read from and at what offsets
 * within each stripe chunk file.
 *
 * @param config Global stripe config.
 * @param layout Per-file stripe layout.
 * @param offset Logical file offset to start reading.
 * @param length Number of bytes to read.
 * @param extents Output array (caller allocates, at least MAX_EXTENTS).
 * @param extent_count Output: number of extents produced.
 * @return 0 on success, -1 on error.
 */
int stripe_map_read(stripe_config_t *config, stripe_layout_t *layout, off_t offset, size_t length,
                    stripe_extent_t *extents, int *extent_count);

/**
 * Map a logical write region to a list of stripe extents.
 * Same semantics as stripe_map_read but for writes.
 */
int stripe_map_write(stripe_config_t *config, stripe_layout_t *layout, off_t offset, size_t length,
                     stripe_extent_t *extents, int *extent_count);

/**
 * Build the full path for a stripe chunk file on a specific OST.
 * Format: <ost_path>/<relative_path>.s<stripe_index>
 *
 * @param config Stripe config.
 * @param rel_path Relative file path within the filesystem.
 * @param ost_index Which OST.
 * @param stripe_index Which stripe chunk.
 * @param buf Output buffer for the path.
 * @param buf_size Size of output buffer.
 * @return 0 on success, -1 on error.
 */
int stripe_chunk_path(stripe_config_t *config, const char *rel_path, int ost_index,
                      int stripe_index, char *buf, size_t buf_size);

/**
 * Build the path to a file's stripe metadata file.
 * Format: <ost0_path>/.stripe_meta/<relative_path>.layout
 */
int stripe_meta_path(stripe_config_t *config, const char *rel_path, char *buf, size_t buf_size);

/**
 * Build the directory path for a relative directory on a specific OST.
 * Format: <ost_path>/<relative_dir_path>
 */
int stripe_dir_path(stripe_config_t *config, const char *rel_path, int ost_index, char *buf,
                    size_t buf_size);

/**
 * Save a stripe layout to disk (as a metadata file on OST0).
 */
int stripe_save_layout(stripe_config_t *config, const char *rel_path, stripe_layout_t *layout);

/**
 * Load a stripe layout from disk. Returns NULL if the file doesn't exist.
 * Caller must free the returned layout.
 */
stripe_layout_t *stripe_load_layout(stripe_config_t *config, const char *rel_path);

/**
 * Delete a file's stripe layout metadata and all stripe chunks across all OSTs.
 */
int stripe_delete_file(stripe_config_t *config, const char *rel_path, stripe_layout_t *layout);

/**
 * Compute the total file size by examining stripe chunks on disk.
 * Returns -1 on error.
 */
ssize_t stripe_compute_size(stripe_config_t *config, const char *rel_path,
                            stripe_layout_t *layout);

/**
 * Create the metadata directory structure for a given path.
 * Ensures parent directories for .stripe_meta files exist.
 */
int stripe_ensure_meta_dir(stripe_config_t *config, const char *rel_path);

/**
 * Create a directory on all OSTs plus the metadata directory.
 */
int stripe_mkdir(stripe_config_t *config, const char *rel_path, mode_t mode);

/**
 * Remove a directory from all OSTs.
 */
int stripe_rmdir(stripe_config_t *config, const char *rel_path);

#endif /* STRIPEFS_STRIPE_H */
