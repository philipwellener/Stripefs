#ifndef STRIPEFS_OPS_H
#define STRIPEFS_OPS_H

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>

#include "cache.h"
#include "stats.h"
#include "stripe.h"

/**
 * Global filesystem context, passed through FUSE's private_data mechanism.
 * Created in main() and accessible in all callbacks via fuse_get_context().
 */
typedef struct stripefs_context {
    stripe_config_t *stripe_config;
    cache_t *cache;
    stats_t *stats;
    int verbose;
} stripefs_context_t;

/** Path to the virtual stats file exposed at the mountpoint root. */
#define STATS_VIRTUAL_PATH "/.stripefs_stats"

/**
 * Return the FUSE operations structure with all callbacks populated.
 * Called by main() to register our filesystem handlers.
 */
struct fuse_operations *stripefs_get_ops(void);

#endif /* STRIPEFS_OPS_H */
