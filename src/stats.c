#include "stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

stats_t *stats_init(int ost_count) {
    if (ost_count <= 0 || ost_count > MAX_OSTS) {
        return NULL;
    }

    stats_t *stats = calloc(1, sizeof(stats_t));
    if (!stats) {
        return NULL;
    }

    stats->ost_count = ost_count;
    return stats;
}

void stats_destroy(stats_t *stats) {
    free(stats);
}

void stats_record_cache_hit(stats_t *stats, size_t bytes) {
    if (!stats) return;
    atomic_fetch_add(&stats->cache_hits, 1);
    atomic_fetch_add(&stats->bytes_served_from_cache, bytes);
}

void stats_record_cache_miss(stats_t *stats) {
    if (!stats) return;
    atomic_fetch_add(&stats->cache_misses, 1);
}

void stats_record_eviction(stats_t *stats) {
    if (!stats) return;
    atomic_fetch_add(&stats->evictions, 1);
}

void stats_record_ost_read(stats_t *stats, int ost_index, size_t bytes) {
    if (!stats || ost_index < 0 || ost_index >= stats->ost_count) return;
    atomic_fetch_add(&stats->ost[ost_index].bytes_read, bytes);
    atomic_fetch_add(&stats->ost[ost_index].read_ops, 1);
    atomic_fetch_add(&stats->bytes_read_from_osts, bytes);
}

void stats_record_ost_write(stats_t *stats, int ost_index, size_t bytes) {
    if (!stats || ost_index < 0 || ost_index >= stats->ost_count) return;
    atomic_fetch_add(&stats->ost[ost_index].bytes_written, bytes);
    atomic_fetch_add(&stats->ost[ost_index].write_ops, 1);
    atomic_fetch_add(&stats->bytes_written_to_osts, bytes);
}

void stats_record_read_op(stats_t *stats) {
    if (!stats) return;
    atomic_fetch_add(&stats->total_read_ops, 1);
}

void stats_record_write_op(stats_t *stats) {
    if (!stats) return;
    atomic_fetch_add(&stats->total_write_ops, 1);
}

int stats_to_json(stats_t *stats, char *buf, size_t buf_size) {
    if (!stats || !buf || buf_size == 0) {
        return -1;
    }

    uint64_t hits = atomic_load(&stats->cache_hits);
    uint64_t misses = atomic_load(&stats->cache_misses);
    uint64_t total_lookups = hits + misses;
    double hit_rate = total_lookups > 0 ? (double)hits / (double)total_lookups * 100.0 : 0.0;

    int written = snprintf(buf, buf_size,
                           "{\n"
                           "  \"cache\": {\n"
                           "    \"hits\": %lu,\n"
                           "    \"misses\": %lu,\n"
                           "    \"hit_rate_pct\": %.2f,\n"
                           "    \"evictions\": %lu,\n"
                           "    \"bytes_served\": %lu\n"
                           "  },\n"
                           "  \"io\": {\n"
                           "    \"total_read_ops\": %lu,\n"
                           "    \"total_write_ops\": %lu,\n"
                           "    \"bytes_read_from_osts\": %lu,\n"
                           "    \"bytes_written_to_osts\": %lu\n"
                           "  },\n"
                           "  \"osts\": [\n",
                           (unsigned long)hits, (unsigned long)misses, hit_rate,
                           (unsigned long)atomic_load(&stats->evictions),
                           (unsigned long)atomic_load(&stats->bytes_served_from_cache),
                           (unsigned long)atomic_load(&stats->total_read_ops),
                           (unsigned long)atomic_load(&stats->total_write_ops),
                           (unsigned long)atomic_load(&stats->bytes_read_from_osts),
                           (unsigned long)atomic_load(&stats->bytes_written_to_osts));

    if (written < 0 || (size_t)written >= buf_size) {
        return -1;
    }

    for (int i = 0; i < stats->ost_count; i++) {
        int n = snprintf(buf + written, buf_size - (size_t)written,
                         "    {\n"
                         "      \"ost_index\": %d,\n"
                         "      \"bytes_read\": %lu,\n"
                         "      \"bytes_written\": %lu,\n"
                         "      \"read_ops\": %lu,\n"
                         "      \"write_ops\": %lu\n"
                         "    }%s\n",
                         i,
                         (unsigned long)atomic_load(&stats->ost[i].bytes_read),
                         (unsigned long)atomic_load(&stats->ost[i].bytes_written),
                         (unsigned long)atomic_load(&stats->ost[i].read_ops),
                         (unsigned long)atomic_load(&stats->ost[i].write_ops),
                         i < stats->ost_count - 1 ? "," : "");
        if (n < 0 || (size_t)n >= buf_size - (size_t)written) {
            return -1;
        }
        written += n;
    }

    int n = snprintf(buf + written, buf_size - (size_t)written, "  ]\n}\n");
    if (n < 0 || (size_t)n >= buf_size - (size_t)written) {
        return -1;
    }
    written += n;

    return written;
}

void stats_print_summary(stats_t *stats) {
    if (!stats) return;

    uint64_t hits = atomic_load(&stats->cache_hits);
    uint64_t misses = atomic_load(&stats->cache_misses);
    uint64_t total = hits + misses;
    double hit_rate = total > 0 ? (double)hits / (double)total * 100.0 : 0.0;

    fprintf(stderr, "\n=== stripefs statistics ===\n");
    fprintf(stderr, "Cache: %lu hits, %lu misses (%.1f%% hit rate), %lu evictions\n",
            (unsigned long)hits, (unsigned long)misses, hit_rate,
            (unsigned long)atomic_load(&stats->evictions));
    fprintf(stderr, "I/O: %lu reads, %lu writes\n",
            (unsigned long)atomic_load(&stats->total_read_ops),
            (unsigned long)atomic_load(&stats->total_write_ops));
    fprintf(stderr, "Bytes: %lu from cache, %lu from OSTs, %lu written to OSTs\n",
            (unsigned long)atomic_load(&stats->bytes_served_from_cache),
            (unsigned long)atomic_load(&stats->bytes_read_from_osts),
            (unsigned long)atomic_load(&stats->bytes_written_to_osts));

    fprintf(stderr, "Per-OST utilization:\n");
    for (int i = 0; i < stats->ost_count; i++) {
        fprintf(stderr, "  OST%d: read %lu bytes (%lu ops), wrote %lu bytes (%lu ops)\n", i,
                (unsigned long)atomic_load(&stats->ost[i].bytes_read),
                (unsigned long)atomic_load(&stats->ost[i].read_ops),
                (unsigned long)atomic_load(&stats->ost[i].bytes_written),
                (unsigned long)atomic_load(&stats->ost[i].write_ops));
    }
    fprintf(stderr, "===========================\n\n");
}
