#ifndef STRIPEFS_STATS_H
#define STRIPEFS_STATS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum number of OSTs supported. */
#define MAX_OSTS 64

/**
 * Per-OST I/O counters for tracking utilization across storage targets.
 */
typedef struct ost_stats {
    atomic_uint_fast64_t bytes_read;
    atomic_uint_fast64_t bytes_written;
    atomic_uint_fast64_t read_ops;
    atomic_uint_fast64_t write_ops;
} ost_stats_t;

/**
 * Global telemetry counters. All fields use C11 atomics for lock-free updates
 * from concurrent FUSE threads.
 */
typedef struct stats {
    atomic_uint_fast64_t cache_hits;
    atomic_uint_fast64_t cache_misses;
    atomic_uint_fast64_t evictions;
    atomic_uint_fast64_t bytes_served_from_cache;
    atomic_uint_fast64_t bytes_read_from_osts;
    atomic_uint_fast64_t bytes_written_to_osts;
    atomic_uint_fast64_t total_read_ops;
    atomic_uint_fast64_t total_write_ops;
    ost_stats_t ost[MAX_OSTS];
    int ost_count;
} stats_t;

/**
 * Initialize a stats structure with all counters zeroed.
 * @param ost_count Number of OSTs to track (must be <= MAX_OSTS).
 * @return Heap-allocated stats, or NULL on failure.
 */
stats_t *stats_init(int ost_count);

/** Free a stats structure. */
void stats_destroy(stats_t *stats);

/** Record a cache hit for a read of `bytes` size. */
void stats_record_cache_hit(stats_t *stats, size_t bytes);

/** Record a cache miss. */
void stats_record_cache_miss(stats_t *stats);

/** Record a cache eviction. */
void stats_record_eviction(stats_t *stats);

/** Record bytes read from a specific OST. */
void stats_record_ost_read(stats_t *stats, int ost_index, size_t bytes);

/** Record bytes written to a specific OST. */
void stats_record_ost_write(stats_t *stats, int ost_index, size_t bytes);

/** Record a read operation (filesystem-level). */
void stats_record_read_op(stats_t *stats);

/** Record a write operation (filesystem-level). */
void stats_record_write_op(stats_t *stats);

/**
 * Render current stats as a JSON string.
 * @param stats The stats structure.
 * @param buf Output buffer.
 * @param buf_size Size of output buffer.
 * @return Number of bytes written, or -1 on error.
 */
int stats_to_json(stats_t *stats, char *buf, size_t buf_size);

/** Print a summary of stats to stderr (called on unmount). */
void stats_print_summary(stats_t *stats);

#endif /* STRIPEFS_STATS_H */
