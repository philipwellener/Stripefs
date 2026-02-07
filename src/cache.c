#include "cache.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/**
 * Remove an entry from the LRU doubly-linked list (does not free or unhash).
 * Caller must hold the write lock.
 */
static void lru_remove(cache_t *cache, cache_entry_t *entry) {
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache->head = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache->tail = entry->prev;
    }

    entry->prev = NULL;
    entry->next = NULL;
}

/**
 * Move an entry to the head (MRU position) of the LRU list.
 * Caller must hold the write lock.
 */
static void lru_move_to_head(cache_t *cache, cache_entry_t *entry) {
    if (cache->head == entry) {
        return; /* Already at head */
    }

    /* Remove from current position */
    lru_remove(cache, entry);

    /* Insert at head */
    entry->next = cache->head;
    entry->prev = NULL;

    if (cache->head) {
        cache->head->prev = entry;
    }
    cache->head = entry;

    if (!cache->tail) {
        cache->tail = entry;
    }
}

/**
 * Free a cache entry's resources (path, data). Does not unlink from list or hash.
 */
static void entry_free(cache_entry_t *entry) {
    if (!entry) return;
    free(entry->path);
    free(entry->data);
    free(entry);
}

/**
 * Remove and free the LRU (tail) entry. Caller must hold the write lock.
 * Returns the number of bytes freed.
 */
static size_t evict_one(cache_t *cache) {
    cache_entry_t *victim = cache->tail;
    if (!victim) return 0;

    size_t freed = victim->size;

    lru_remove(cache, victim);
    HASH_DEL(cache->entries, victim);
    cache->current_size -= freed;

    entry_free(victim);
    return freed;
}

cache_t *cache_init(size_t max_size, int ttl_seconds) {
    cache_t *cache = calloc(1, sizeof(cache_t));
    if (!cache) return NULL;

    cache->max_size = max_size;
    cache->ttl_seconds = ttl_seconds;

    if (pthread_rwlock_init(&cache->lock, NULL) != 0) {
        free(cache);
        return NULL;
    }

    return cache;
}

void cache_destroy(cache_t *cache) {
    if (!cache) return;

    pthread_rwlock_wrlock(&cache->lock);

    cache_entry_t *entry, *tmp;
    HASH_ITER(hh, cache->entries, entry, tmp) {
        HASH_DEL(cache->entries, entry);
        entry_free(entry);
    }

    cache->head = NULL;
    cache->tail = NULL;
    cache->current_size = 0;

    pthread_rwlock_unlock(&cache->lock);
    pthread_rwlock_destroy(&cache->lock);
    free(cache);
}

ssize_t cache_get(cache_t *cache, const char *path, void *buf, size_t buf_size,
                  size_t *out_total) {
    if (!cache || !path) return -1;

    /* Take write lock because a hit modifies the LRU list */
    pthread_rwlock_wrlock(&cache->lock);

    cache_entry_t *entry = NULL;
    HASH_FIND_STR(cache->entries, path, entry);

    if (!entry) {
        pthread_rwlock_unlock(&cache->lock);
        return -1;
    }

    /* Check TTL expiration */
    if (cache->ttl_seconds > 0) {
        time_t now = time(NULL);
        if (now - entry->cached_at >= cache->ttl_seconds) {
            /* Expired — evict and return miss */
            lru_remove(cache, entry);
            HASH_DEL(cache->entries, entry);
            cache->current_size -= entry->size;
            entry_free(entry);
            pthread_rwlock_unlock(&cache->lock);
            return -1;
        }
    }

    /* Cache hit — move to MRU position */
    lru_move_to_head(cache, entry);

    if (out_total) {
        *out_total = entry->size;
    }

    /* Copy data into caller's buffer while holding the lock */
    ssize_t copied = 0;
    if (buf && buf_size > 0) {
        size_t to_copy = entry->size < buf_size ? entry->size : buf_size;
        memcpy(buf, entry->data, to_copy);
        copied = (ssize_t)to_copy;
    }

    pthread_rwlock_unlock(&cache->lock);
    return copied;
}

int cache_put(cache_t *cache, const char *path, const void *data, size_t size) {
    if (!cache || !path || !data || size == 0) return -1;

    /* Don't cache items larger than the entire cache */
    if (size > cache->max_size) return -1;

    pthread_rwlock_wrlock(&cache->lock);

    /* Check if entry already exists — update in place */
    cache_entry_t *existing = NULL;
    HASH_FIND_STR(cache->entries, path, existing);

    if (existing) {
        /* Update existing entry */
        void *new_data = malloc(size);
        if (!new_data) {
            pthread_rwlock_unlock(&cache->lock);
            return -1;
        }
        memcpy(new_data, data, size);

        cache->current_size -= existing->size;
        free(existing->data);
        existing->data = new_data;
        existing->size = size;
        existing->cached_at = time(NULL);
        cache->current_size += size;

        lru_move_to_head(cache, existing);

        /* Evict if over capacity */
        while (cache->current_size > cache->max_size && cache->tail) {
            evict_one(cache);
        }

        pthread_rwlock_unlock(&cache->lock);
        return 0;
    }

    /* Evict entries to make room for the new one */
    while (cache->current_size + size > cache->max_size && cache->tail) {
        evict_one(cache);
    }

    /* Create new entry */
    cache_entry_t *entry = calloc(1, sizeof(cache_entry_t));
    if (!entry) {
        pthread_rwlock_unlock(&cache->lock);
        return -1;
    }

    entry->path = strdup(path);
    if (!entry->path) {
        free(entry);
        pthread_rwlock_unlock(&cache->lock);
        return -1;
    }

    entry->data = malloc(size);
    if (!entry->data) {
        free(entry->path);
        free(entry);
        pthread_rwlock_unlock(&cache->lock);
        return -1;
    }
    memcpy(entry->data, data, size);

    entry->size = size;
    entry->cached_at = time(NULL);

    /* Add to hash table */
    HASH_ADD_KEYPTR(hh, cache->entries, entry->path, strlen(entry->path), entry);

    /* Add to head of LRU list */
    entry->next = cache->head;
    entry->prev = NULL;
    if (cache->head) {
        cache->head->prev = entry;
    }
    cache->head = entry;
    if (!cache->tail) {
        cache->tail = entry;
    }

    cache->current_size += size;

    pthread_rwlock_unlock(&cache->lock);
    return 0;
}

void cache_invalidate(cache_t *cache, const char *path) {
    if (!cache || !path) return;

    pthread_rwlock_wrlock(&cache->lock);

    cache_entry_t *entry = NULL;
    HASH_FIND_STR(cache->entries, path, entry);

    if (entry) {
        cache->current_size -= entry->size;
        lru_remove(cache, entry);
        HASH_DEL(cache->entries, entry);
        entry_free(entry);
    }

    pthread_rwlock_unlock(&cache->lock);
}

void cache_invalidate_prefix(cache_t *cache, const char *prefix) {
    if (!cache || !prefix) return;

    size_t prefix_len = strlen(prefix);

    pthread_rwlock_wrlock(&cache->lock);

    cache_entry_t *entry, *tmp;
    HASH_ITER(hh, cache->entries, entry, tmp) {
        if (strncmp(entry->path, prefix, prefix_len) == 0) {
            cache->current_size -= entry->size;
            lru_remove(cache, entry);
            HASH_DEL(cache->entries, entry);
            entry_free(entry);
        }
    }

    pthread_rwlock_unlock(&cache->lock);
}
