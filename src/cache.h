#ifndef STRIPEFS_CACHE_H
#define STRIPEFS_CACHE_H

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "uthash.h"

/**
 * A single entry in the LRU cache. Each entry stores cached file content keyed
 * by path. Entries are linked in a doubly-linked list for LRU ordering and
 * indexed by a hash table (uthash) for O(1) lookups.
 */
typedef struct cache_entry {
    char *path;              /* File path (hash key) */
    void *data;              /* Cached file content */
    size_t size;             /* Size of cached data in bytes */
    time_t cached_at;        /* Timestamp when entry was cached (for TTL) */

    struct cache_entry *prev; /* LRU list: toward MRU */
    struct cache_entry *next; /* LRU list: toward LRU */

    UT_hash_handle hh;       /* uthash handle for O(1) path lookup */
} cache_entry_t;

/**
 * Thread-safe LRU cache with TTL expiration. Uses a hash table for O(1) lookups
 * and a doubly-linked list for O(1) LRU eviction.
 */
typedef struct cache {
    cache_entry_t *entries;   /* uthash hash table head */
    cache_entry_t *head;      /* Most recently used entry */
    cache_entry_t *tail;      /* Least recently used entry */
    size_t current_size;      /* Total bytes currently cached */
    size_t max_size;          /* Maximum cache size in bytes */
    int ttl_seconds;          /* Time-to-live before entry expires */
    pthread_rwlock_t lock;    /* Read-write lock for thread safety */
} cache_t;

/**
 * Initialize a new cache.
 * @param max_size Maximum cache size in bytes.
 * @param ttl_seconds TTL for cache entries (0 = no expiry).
 * @return Heap-allocated cache, or NULL on failure.
 */
cache_t *cache_init(size_t max_size, int ttl_seconds);

/** Destroy a cache and free all entries. */
void cache_destroy(cache_t *cache);

/**
 * Copy cached data into a caller-provided buffer. Moves entry to MRU position
 * on hit. Returns the number of bytes copied, or -1 on miss / expired TTL.
 *
 * This copies data while holding the lock, so the caller does not hold a
 * dangling pointer into cache-owned memory.
 *
 * @param cache The cache.
 * @param path File path to look up.
 * @param buf Caller-provided output buffer (may be NULL to query size only).
 * @param buf_size Size of caller's buffer.
 * @param out_total If non-NULL, set to the total cached entry size on hit.
 * @return Bytes copied on hit (may be less than total if buf_size is smaller),
 *         or -1 on miss.
 */
ssize_t cache_get(cache_t *cache, const char *path, void *buf, size_t buf_size,
                  size_t *out_total);

/**
 * Insert or update a cache entry. Evicts LRU entries if necessary to stay
 * within max_size. Makes a copy of the provided data.
 * @param cache The cache.
 * @param path File path (key).
 * @param data Data to cache (will be copied).
 * @param size Size of data in bytes.
 * @return 0 on success, -1 on failure.
 */
int cache_put(cache_t *cache, const char *path, const void *data, size_t size);

/**
 * Invalidate (remove) a specific cache entry.
 * @param cache The cache.
 * @param path File path to invalidate.
 */
void cache_invalidate(cache_t *cache, const char *path);

/**
 * Invalidate all entries whose path starts with the given prefix.
 * Useful for directory-level invalidation.
 */
void cache_invalidate_prefix(cache_t *cache, const char *prefix);

#endif /* STRIPEFS_CACHE_H */
