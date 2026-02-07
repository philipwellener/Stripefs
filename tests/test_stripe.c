#include "stripe.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        printf("  %-50s ", #name);                                                                 \
        fflush(stdout);                                                                            \
    } while (0)

#define PASS()                                                                                     \
    do {                                                                                           \
        printf("PASS\n");                                                                          \
        tests_passed++;                                                                            \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        printf("FAIL: %s\n", msg);                                                                 \
        tests_failed++;                                                                            \
    } while (0)

#define ASSERT_EQ(a, b, msg)                                                                       \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            FAIL(msg);                                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NOT_NULL(ptr, msg)                                                                  \
    do {                                                                                           \
        if ((ptr) == NULL) {                                                                       \
            FAIL(msg);                                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static char test_dir[128];
static char ost_dirs[4][256];
static const char *ost_paths[4];

/** Create temp directories for each test. */
static void setup_test_dirs(void) {
    snprintf(test_dir, sizeof(test_dir), "/tmp/sfs_test_%d", getpid());
    mkdir(test_dir, 0755);

    for (int i = 0; i < 4; i++) {
        snprintf(ost_dirs[i], sizeof(ost_dirs[i]), "%s/ost%d", test_dir, i);
        mkdir(ost_dirs[i], 0755);
        ost_paths[i] = ost_dirs[i];
    }
}

/** Recursive remove of test directories. */
static void cleanup_test_dirs(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    if (system(cmd) != 0) {
        /* ignore cleanup errors */
    }
}

static void test_init_destroy(void) {
    TEST(test_init_destroy);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);
    ASSERT_NOT_NULL(config, "stripe_init returned NULL");
    ASSERT_EQ(config->ost_count, 4, "ost_count mismatch");
    ASSERT_EQ(config->stripe_size, 1024UL, "stripe_size mismatch");
    stripe_destroy(config);
    PASS();
}

static void test_map_read_single_stripe(void) {
    TEST(test_map_read_single_stripe);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 4096, .start_ost = 0};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    /* Read 500 bytes from offset 0 — should be a single extent on OST0 */
    stripe_map_read(config, &layout, 0, 500, extents, &count);
    ASSERT_EQ(count, 1, "expected 1 extent");
    ASSERT_EQ(extents[0].ost_index, 0, "expected OST0");
    ASSERT_EQ(extents[0].stripe_index, 0, "expected stripe 0");
    ASSERT_EQ(extents[0].local_offset, 0L, "expected offset 0");
    ASSERT_EQ(extents[0].length, 500UL, "expected 500 bytes");

    stripe_destroy(config);
    PASS();
}

static void test_map_read_crosses_stripe(void) {
    TEST(test_map_read_crosses_stripe);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 8192, .start_ost = 0};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    /* Read 1500 bytes from offset 512 — crosses from stripe 0 to stripe 1 */
    stripe_map_read(config, &layout, 512, 1500, extents, &count);
    ASSERT_EQ(count, 2, "expected 2 extents");

    /* First extent: stripe 0 on OST0, from offset 512, 512 bytes */
    ASSERT_EQ(extents[0].ost_index, 0, "ext0 OST");
    ASSERT_EQ(extents[0].stripe_index, 0, "ext0 stripe");
    ASSERT_EQ(extents[0].local_offset, 512L, "ext0 offset");
    ASSERT_EQ(extents[0].length, 512UL, "ext0 length");

    /* Second extent: stripe 1 on OST1, from offset 0, 988 bytes */
    ASSERT_EQ(extents[1].ost_index, 1, "ext1 OST");
    ASSERT_EQ(extents[1].stripe_index, 1, "ext1 stripe");
    ASSERT_EQ(extents[1].local_offset, 0L, "ext1 offset");
    ASSERT_EQ(extents[1].length, 988UL, "ext1 length");

    stripe_destroy(config);
    PASS();
}

static void test_map_read_multiple_stripes(void) {
    TEST(test_map_read_multiple_stripes);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    /* 5KB file: stripes 0-4 across OST0-3 then wrap to OST0 */
    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 5120, .start_ost = 0};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    /* Read entire file */
    stripe_map_read(config, &layout, 0, 5120, extents, &count);
    ASSERT_EQ(count, 5, "expected 5 extents");

    /* Verify round-robin OST assignment */
    ASSERT_EQ(extents[0].ost_index, 0, "stripe 0 on OST0");
    ASSERT_EQ(extents[1].ost_index, 1, "stripe 1 on OST1");
    ASSERT_EQ(extents[2].ost_index, 2, "stripe 2 on OST2");
    ASSERT_EQ(extents[3].ost_index, 3, "stripe 3 on OST3");
    ASSERT_EQ(extents[4].ost_index, 0, "stripe 4 wraps to OST0");

    /* Each extent should be a full stripe except possibly the last */
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(extents[i].length, 1024UL, "each extent should be full stripe");
    }

    stripe_destroy(config);
    PASS();
}

static void test_map_read_clamps_to_eof(void) {
    TEST(test_map_read_clamps_to_eof);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 1500, .start_ost = 0};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    /* Request more than the file contains */
    stripe_map_read(config, &layout, 0, 10000, extents, &count);
    ASSERT_EQ(count, 2, "expected 2 extents");
    ASSERT_EQ(extents[0].length, 1024UL, "first stripe full");
    ASSERT_EQ(extents[1].length, 476UL, "second stripe partial");

    /* Total mapped bytes should equal file size */
    size_t total = extents[0].length + extents[1].length;
    ASSERT_EQ(total, 1500UL, "total should equal file size");

    stripe_destroy(config);
    PASS();
}

static void test_map_read_past_eof(void) {
    TEST(test_map_read_past_eof);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 100, .start_ost = 0};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    /* Read starting past EOF */
    stripe_map_read(config, &layout, 200, 50, extents, &count);
    ASSERT_EQ(count, 0, "expected 0 extents past EOF");

    stripe_destroy(config);
    PASS();
}

static void test_map_with_start_ost(void) {
    TEST(test_map_with_start_ost);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    /* File starts on OST2 */
    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 4096, .start_ost = 2};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    stripe_map_read(config, &layout, 0, 4096, extents, &count);
    ASSERT_EQ(count, 4, "expected 4 extents");

    /* Round-robin from OST2: 2, 3, 0, 1 */
    ASSERT_EQ(extents[0].ost_index, 2, "stripe 0 on OST2");
    ASSERT_EQ(extents[1].ost_index, 3, "stripe 1 on OST3");
    ASSERT_EQ(extents[2].ost_index, 0, "stripe 2 wraps to OST0");
    ASSERT_EQ(extents[3].ost_index, 1, "stripe 3 on OST1");

    stripe_destroy(config);
    PASS();
}

static void test_chunk_path(void) {
    TEST(test_chunk_path);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    char buf[4096];
    int rc = stripe_chunk_path(config, "/hello.txt", 1, 3, buf, sizeof(buf));
    ASSERT_EQ(rc, 0, "stripe_chunk_path failed");

    /* Should be: <ost1_path>/hello.txt.s3 */
    char expected[4096];
    snprintf(expected, sizeof(expected), "%s/hello.txt.s3", ost_dirs[1]);
    ASSERT_EQ(strcmp(buf, expected), 0, "chunk path mismatch");

    stripe_destroy(config);
    PASS();
}

static void test_save_load_layout(void) {
    TEST(test_save_load_layout);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 5000, .start_ost = 2};

    int rc = stripe_save_layout(config, "/testfile.txt", &layout);
    ASSERT_EQ(rc, 0, "save layout failed");

    stripe_layout_t *loaded = stripe_load_layout(config, "/testfile.txt");
    ASSERT_NOT_NULL(loaded, "load layout returned NULL");
    ASSERT_EQ(loaded->ost_count, 4, "loaded ost_count");
    ASSERT_EQ(loaded->stripe_size, 1024UL, "loaded stripe_size");
    ASSERT_EQ(loaded->total_size, 5000UL, "loaded total_size");
    ASSERT_EQ(loaded->start_ost, 2, "loaded start_ost");

    free(loaded);
    stripe_destroy(config);
    PASS();
}

static void test_write_map(void) {
    TEST(test_write_map);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 0, .start_ost = 0};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    /* Write 3000 bytes from offset 0 */
    stripe_map_write(config, &layout, 0, 3000, extents, &count);
    ASSERT_EQ(count, 3, "expected 3 extents for 3000 bytes");
    ASSERT_EQ(extents[0].length, 1024UL, "first stripe full");
    ASSERT_EQ(extents[1].length, 1024UL, "second stripe full");
    ASSERT_EQ(extents[2].length, 952UL, "third stripe partial");

    stripe_destroy(config);
    PASS();
}

static void test_map_aligned_boundary(void) {
    TEST(test_map_aligned_boundary);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    /* File size is exactly 2 stripes */
    stripe_layout_t layout = {.ost_count = 4, .stripe_size = 1024, .total_size = 2048, .start_ost = 0};

    stripe_extent_t extents[MAX_EXTENTS];
    int count = 0;

    /* Read at exact stripe boundary */
    stripe_map_read(config, &layout, 1024, 1024, extents, &count);
    ASSERT_EQ(count, 1, "expected 1 extent at boundary");
    ASSERT_EQ(extents[0].ost_index, 1, "should be on OST1");
    ASSERT_EQ(extents[0].local_offset, 0L, "should start at 0 within stripe");
    ASSERT_EQ(extents[0].length, 1024UL, "full stripe");

    stripe_destroy(config);
    PASS();
}

static void test_mkdir_rmdir(void) {
    TEST(test_mkdir_rmdir);
    stripe_config_t *config = stripe_init(4, 1024, ost_paths);

    int rc = stripe_mkdir(config, "/testdir", 0755);
    ASSERT_EQ(rc, 0, "mkdir failed");

    /* Verify directory exists on all OSTs */
    char path[4096];
    struct stat st;
    for (int i = 0; i < 4; i++) {
        stripe_dir_path(config, "/testdir", i, path, sizeof(path));
        ASSERT_EQ(stat(path, &st), 0, "dir should exist on OST");
    }

    rc = stripe_rmdir(config, "/testdir");
    ASSERT_EQ(rc, 0, "rmdir failed");

    stripe_destroy(config);
    PASS();
}

int main(void) {
    printf("=== Stripe Unit Tests ===\n");

    setup_test_dirs();

    test_init_destroy();
    test_map_read_single_stripe();
    test_map_read_crosses_stripe();
    test_map_read_multiple_stripes();
    test_map_read_clamps_to_eof();
    test_map_read_past_eof();
    test_map_with_start_ost();
    test_chunk_path();
    test_save_load_layout();
    test_write_map();
    test_map_aligned_boundary();
    test_mkdir_rmdir();

    cleanup_test_dirs();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
