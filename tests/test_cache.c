#include "cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            FAIL(msg);                                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/** Helper: cache_get that returns >= 0 on hit, -1 on miss. */
static int cache_hit(cache_t *c, const char *path) {
    return cache_get(c, path, NULL, 0, NULL) >= 0 ? 1 : 0;
}

static void test_init_destroy(void) {
    TEST(test_init_destroy);
    cache_t *c = cache_init(1024, 30);
    ASSERT_TRUE(c != NULL, "cache_init returned NULL");
    ASSERT_EQ(c->max_size, 1024UL, "max_size mismatch");
    ASSERT_EQ(c->ttl_seconds, 30, "ttl mismatch");
    ASSERT_EQ(c->current_size, 0UL, "current_size should be 0");
    cache_destroy(c);
    PASS();
}

static void test_put_and_get(void) {
    TEST(test_put_and_get);
    cache_t *c = cache_init(4096, 0);
    const char *data = "hello world";
    size_t len = strlen(data) + 1;

    int rc = cache_put(c, "/foo/bar.txt", data, len);
    ASSERT_EQ(rc, 0, "cache_put failed");

    char buf[256];
    size_t out_total = 0;
    ssize_t got = cache_get(c, "/foo/bar.txt", buf, sizeof(buf), &out_total);
    ASSERT_TRUE(got >= 0, "cache_get returned miss");
    ASSERT_EQ(out_total, len, "total size mismatch");
    ASSERT_EQ((size_t)got, len, "copied size mismatch");
    ASSERT_EQ(memcmp(buf, data, len), 0, "data mismatch");

    cache_destroy(c);
    PASS();
}

static void test_miss(void) {
    TEST(test_miss);
    cache_t *c = cache_init(4096, 0);
    ssize_t got = cache_get(c, "/nonexistent", NULL, 0, NULL);
    ASSERT_TRUE(got < 0, "expected miss for nonexistent key");
    cache_destroy(c);
    PASS();
}

static void test_invalidate(void) {
    TEST(test_invalidate);
    cache_t *c = cache_init(4096, 0);

    cache_put(c, "/a", "data_a", 7);
    cache_put(c, "/b", "data_b", 7);

    cache_invalidate(c, "/a");

    ASSERT_TRUE(!cache_hit(c, "/a"), "expected miss after invalidate");
    ASSERT_TRUE(cache_hit(c, "/b"), "expected /b to still exist");

    cache_destroy(c);
    PASS();
}

static void test_invalidate_prefix(void) {
    TEST(test_invalidate_prefix);
    cache_t *c = cache_init(4096, 0);

    cache_put(c, "/dir/a", "aa", 3);
    cache_put(c, "/dir/b", "bb", 3);
    cache_put(c, "/other/c", "cc", 3);

    cache_invalidate_prefix(c, "/dir/");

    ASSERT_TRUE(!cache_hit(c, "/dir/a"), "expected /dir/a invalidated");
    ASSERT_TRUE(!cache_hit(c, "/dir/b"), "expected /dir/b invalidated");
    ASSERT_TRUE(cache_hit(c, "/other/c"), "expected /other/c to remain");

    cache_destroy(c);
    PASS();
}

static void test_lru_eviction(void) {
    TEST(test_lru_eviction);
    /* Cache with room for exactly 3 entries of 100 bytes each */
    cache_t *c = cache_init(300, 0);

    char buf[100];
    memset(buf, 'A', sizeof(buf));

    cache_put(c, "/a", buf, 100); /* oldest */
    cache_put(c, "/b", buf, 100);
    cache_put(c, "/c", buf, 100); /* newest */

    ASSERT_EQ(c->current_size, 300UL, "should be full");

    /* Adding /d should evict /a (LRU) */
    cache_put(c, "/d", buf, 100);

    ASSERT_TRUE(!cache_hit(c, "/a"), "/a should have been evicted");
    ASSERT_TRUE(cache_hit(c, "/b"), "/b should remain");
    ASSERT_TRUE(cache_hit(c, "/c"), "/c should remain");
    ASSERT_TRUE(cache_hit(c, "/d"), "/d should exist");

    cache_destroy(c);
    PASS();
}

static void test_lru_access_reorder(void) {
    TEST(test_lru_access_reorder);
    /* Accessing an entry moves it to MRU, so the next eviction skips it. */
    cache_t *c = cache_init(300, 0);

    char buf[100];
    memset(buf, 'X', sizeof(buf));

    cache_put(c, "/a", buf, 100);
    cache_put(c, "/b", buf, 100);
    cache_put(c, "/c", buf, 100);

    /* Access /a — moves it to MRU position */
    cache_get(c, "/a", NULL, 0, NULL);

    /* Adding /d should evict /b (now LRU, since /a was accessed) */
    cache_put(c, "/d", buf, 100);

    ASSERT_TRUE(cache_hit(c, "/a"), "/a should remain (was accessed)");
    ASSERT_TRUE(!cache_hit(c, "/b"), "/b should have been evicted");
    ASSERT_TRUE(cache_hit(c, "/c"), "/c should remain");
    ASSERT_TRUE(cache_hit(c, "/d"), "/d should exist");

    cache_destroy(c);
    PASS();
}

static void test_ttl_expiration(void) {
    TEST(test_ttl_expiration);
    /* TTL of 1 second */
    cache_t *c = cache_init(4096, 1);

    cache_put(c, "/ttl_test", "data", 5);

    /* Should be available immediately */
    ASSERT_TRUE(cache_hit(c, "/ttl_test"), "should exist before TTL");

    /* Wait for TTL to expire */
    sleep(2);

    ASSERT_TRUE(!cache_hit(c, "/ttl_test"), "should be miss after TTL");

    cache_destroy(c);
    PASS();
}

static void test_update_existing(void) {
    TEST(test_update_existing);
    cache_t *c = cache_init(4096, 0);

    cache_put(c, "/f", "old_data", 9);
    cache_put(c, "/f", "new_data_longer", 16);

    char buf[256];
    size_t out_total = 0;
    ssize_t got = cache_get(c, "/f", buf, sizeof(buf), &out_total);
    ASSERT_TRUE(got >= 0, "should exist after update");
    ASSERT_EQ(out_total, 16UL, "size should reflect new data");
    ASSERT_EQ(memcmp(buf, "new_data_longer", 16), 0, "data should be updated");

    cache_destroy(c);
    PASS();
}

static void test_oversized_entry_rejected(void) {
    TEST(test_oversized_entry_rejected);
    cache_t *c = cache_init(100, 0);

    char big[200];
    memset(big, 'Z', sizeof(big));

    int rc = cache_put(c, "/big", big, sizeof(big));
    ASSERT_EQ(rc, -1, "oversized put should fail");
    ASSERT_EQ(c->current_size, 0UL, "current_size should remain 0");

    cache_destroy(c);
    PASS();
}

static void test_multiple_evictions(void) {
    TEST(test_multiple_evictions);
    /* Cache fits 200 bytes. Insert 2x100, then one 200-byte entry that
     * requires evicting both existing entries. */
    cache_t *c = cache_init(200, 0);

    char buf100[100];
    memset(buf100, 'A', sizeof(buf100));
    char buf200[200];
    memset(buf200, 'B', sizeof(buf200));

    cache_put(c, "/x", buf100, 100);
    cache_put(c, "/y", buf100, 100);

    /* Inserting 200 bytes should evict both /x and /y */
    cache_put(c, "/z", buf200, 200);

    ASSERT_TRUE(!cache_hit(c, "/x"), "/x should be evicted");
    ASSERT_TRUE(!cache_hit(c, "/y"), "/y should be evicted");
    ASSERT_TRUE(cache_hit(c, "/z"), "/z should exist");
    ASSERT_EQ(c->current_size, 200UL, "current_size should be 200");

    cache_destroy(c);
    PASS();
}

int main(void) {
    printf("=== Cache Unit Tests ===\n");

    test_init_destroy();
    test_put_and_get();
    test_miss();
    test_invalidate();
    test_invalidate_prefix();
    test_lru_eviction();
    test_lru_access_reorder();
    test_ttl_expiration();
    test_update_existing();
    test_oversized_entry_rejected();
    test_multiple_evictions();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
