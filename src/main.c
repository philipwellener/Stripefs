#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cache.h"
#include "stats.h"
#include "stripe.h"
#include "stripefs_ops.h"

#define DEFAULT_STRIPE_SIZE (1024 * 1024) /* 1MB, matching Lustre default */
#define DEFAULT_CACHE_SIZE_MB 128
#define DEFAULT_TTL_SECONDS 30
#define MAX_OSTS_CLI 64

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --mount <mountpoint> --ost <dir> [--ost <dir> ...] [options]\n"
            "\n"
            "A FUSE-based striping filesystem that distributes file data across\n"
            "multiple storage targets, modeled on Lustre's architecture.\n"
            "\n"
            "Required:\n"
            "  --mount <path>         Mountpoint directory\n"
            "  --ost <path>           Backing directory (Object Storage Target)\n"
            "                         Specify at least 2 for meaningful striping\n"
            "\n"
            "Options:\n"
            "  --stripe-size <bytes>  Stripe chunk size (default: 1048576 = 1MB)\n"
            "  --cache-size <MB>      Max cache size in MB (default: 128)\n"
            "  --ttl <seconds>        Cache TTL in seconds (default: 30, 0=no expiry)\n"
            "  --verbose              Enable debug logging to stderr\n"
            "  --foreground           Run in foreground (don't daemonize)\n"
            "  --help                 Show this help message\n",
            prog);
}

int main(int argc, char *argv[]) {
    char *mount_point = NULL;
    const char *ost_paths[MAX_OSTS_CLI];
    int ost_count = 0;
    size_t stripe_size = DEFAULT_STRIPE_SIZE;
    size_t cache_size_mb = DEFAULT_CACHE_SIZE_MB;
    int ttl_seconds = DEFAULT_TTL_SECONDS;
    int verbose = 0;
    int foreground = 0;

    static struct option long_options[] = {{"mount", required_argument, 0, 'm'},
                                           {"ost", required_argument, 0, 'o'},
                                           {"stripe-size", required_argument, 0, 's'},
                                           {"cache-size", required_argument, 0, 'c'},
                                           {"ttl", required_argument, 0, 't'},
                                           {"verbose", no_argument, 0, 'v'},
                                           {"foreground", no_argument, 0, 'f'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "m:o:s:c:t:vfh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'm':
            mount_point = optarg;
            break;
        case 'o':
            if (ost_count >= MAX_OSTS_CLI) {
                fprintf(stderr, "Error: too many OST paths (max %d)\n", MAX_OSTS_CLI);
                return 1;
            }
            ost_paths[ost_count++] = optarg;
            break;
        case 's':
            stripe_size = (size_t)atol(optarg);
            if (stripe_size == 0) {
                fprintf(stderr, "Error: invalid stripe size\n");
                return 1;
            }
            break;
        case 'c':
            cache_size_mb = (size_t)atol(optarg);
            break;
        case 't':
            ttl_seconds = atoi(optarg);
            break;
        case 'v':
            verbose = 1;
            break;
        case 'f':
            foreground = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Validate required arguments */
    if (!mount_point) {
        fprintf(stderr, "Error: --mount is required\n");
        print_usage(argv[0]);
        return 1;
    }

    if (ost_count < 1) {
        fprintf(stderr, "Error: at least one --ost is required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Initialize stripe engine */
    stripe_config_t *stripe_config = stripe_init(ost_count, stripe_size, ost_paths);
    if (!stripe_config) {
        fprintf(stderr, "Error: failed to initialize stripe engine\n");
        return 1;
    }

    /* Initialize cache */
    size_t cache_size_bytes = cache_size_mb * 1024 * 1024;
    cache_t *cache = cache_init(cache_size_bytes, ttl_seconds);
    if (!cache) {
        fprintf(stderr, "Error: failed to initialize cache\n");
        stripe_destroy(stripe_config);
        return 1;
    }

    /* Initialize stats */
    stats_t *stats = stats_init(ost_count);
    if (!stats) {
        fprintf(stderr, "Error: failed to initialize stats\n");
        cache_destroy(cache);
        stripe_destroy(stripe_config);
        return 1;
    }

    /* Build filesystem context */
    stripefs_context_t *ctx = calloc(1, sizeof(stripefs_context_t));
    if (!ctx) {
        fprintf(stderr, "Error: failed to allocate context\n");
        stats_destroy(stats);
        cache_destroy(cache);
        stripe_destroy(stripe_config);
        return 1;
    }

    ctx->stripe_config = stripe_config;
    ctx->cache = cache;
    ctx->stats = stats;
    ctx->verbose = verbose;

    /* Print configuration */
    fprintf(stderr, "stripefs starting:\n");
    fprintf(stderr, "  Mount point: %s\n", mount_point);
    fprintf(stderr, "  OST count: %d\n", ost_count);
    for (int i = 0; i < ost_count; i++) {
        fprintf(stderr, "  OST%d: %s\n", i, ost_paths[i]);
    }
    fprintf(stderr, "  Stripe size: %zu bytes\n", stripe_size);
    fprintf(stderr, "  Cache size: %zu MB\n", cache_size_mb);
    fprintf(stderr, "  Cache TTL: %d seconds\n", ttl_seconds);
    fprintf(stderr, "  Verbose: %s\n", verbose ? "yes" : "no");

    /* Create the root directory on each OST if it doesn't exist */
    for (int i = 0; i < ost_count; i++) {
        struct stat st;
        if (stat(ost_paths[i], &st) != 0) {
            fprintf(stderr, "Error: OST directory does not exist: %s\n", ost_paths[i]);
            free(ctx);
            stats_destroy(stats);
            cache_destroy(cache);
            stripe_destroy(stripe_config);
            return 1;
        }
    }

    /* Build FUSE arguments */
    char *fuse_argv[10];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = mount_point;
    if (foreground) {
        fuse_argv[fuse_argc++] = "-f";
    }
    /* Allow other users to access the mount */
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] = "default_permissions";

    /* Run FUSE main loop */
    struct fuse_operations *ops = stripefs_get_ops();
    int ret = fuse_main(fuse_argc, fuse_argv, ops, ctx);

    /* Cleanup — print stats on unmount */
    stats_print_summary(stats);

    free(ctx);
    stats_destroy(stats);
    cache_destroy(cache);
    stripe_destroy(stripe_config);

    return ret;
}
