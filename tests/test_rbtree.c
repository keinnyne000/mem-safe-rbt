/* M0 unit tests: rb_create, rb_insert (BST descent, no red-black fixup),
 * rb_find, rb_destroy, with rb_size as a node-count oracle.
 *
 * Every assertion here is on behavior observable through the public API.
 * Nothing asserts tree shape, depth, or balance: M0 insert builds degenerate
 * spines by construction and M1 will rebalance them, so a shape assertion
 * would have to be rewritten or weakened later. */

/* Source-relative path: the build links every .c in one gcc invocation and
 * resolves -Iinclude against make's CWD, so do not depend on the search path. */
#include "../include/rbtree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- harness ---------------------------------------------------------- */

static int         g_failures;
static const char *g_current_test = "<none>";

static void report_failure(int line, const char *msg) {
    ++g_failures;
    fprintf(stderr, "FAIL %s (line %d): %s\n", g_current_test, line, msg);
}

/* Records the failure and keeps going, so one run reports every broken case
 * instead of dying on the first. */
#define CHECK(cond, msg)                    \
    do {                                    \
        if (!(cond))                        \
            report_failure(__LINE__, (msg)); \
    } while (0)

/* ---- value bookkeeping ------------------------------------------------ */

/* Test-side values use plain malloc/free, never rb_malloc: the interception
 * point in tests/fault_alloc.h is scoped to allocations made by src/, and
 * keeping test allocations out of it leaves its counters free to measure the
 * library alone once fault injection is added. */

#define FREE_LOG_CAP 8

static size_t g_free_calls;
static void  *g_freed[FREE_LOG_CAP];
static size_t g_freed_n;

/* Passed to rb_create as the owning value_free; tallies calls so a test can
 * assert a value was released exactly once, and by whom. */
static void count_free(void *value) {
    ++g_free_calls;
    if (g_freed_n < FREE_LOG_CAP)
        g_freed[g_freed_n++] = value;
    free(value);
}

static void reset_free_log(void) {
    g_free_calls = 0;
    g_freed_n    = 0;
    memset(g_freed, 0, sizeof g_freed);
}

static int *mkval(int v) {
    int *p = malloc(sizeof *p);
    CHECK(p != NULL, "test harness: malloc for a value failed");
    if (p)
        *p = v;
    return p;
}

/* rb_insert takes ownership of the value only on success, so on a reported
 * failure the value is still ours to release. Returning it to the caller's
 * ledger here keeps a red suite leak-free under Valgrind. */
static int insert_owned(rbtree_t *t, const char *key, void *value) {
    int rc = rb_insert(t, key, value);
    if (rc != 0)
        free(value);
    return rc;
}

#define KEY_BUF 32

static void make_key(char *buf, size_t n, int i) {
    snprintf(buf, n, "k%06d", i);
}

/* ---- A. lifecycle ------------------------------------------------------ */

static void test_create_returns_empty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL, "rb_create(NULL) returned NULL");
    if (!t)
        return;
    CHECK(rb_size(t) == 0, "a fresh tree must have size 0");
    rb_destroy(t);
}

static void test_create_with_value_free(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create(count_free) returned NULL");
    if (!t)
        return;
    CHECK(rb_size(t) == 0, "a fresh tree must have size 0");
    rb_destroy(t);
    CHECK(g_free_calls == 0, "destroying an empty tree must free no values");
}

static void test_destroy_null_is_safe(void) {
    rb_destroy(NULL); /* documented NULL-safe; the oracle is not crashing */
}

static void test_trees_are_independent(void) {
    rbtree_t *a = rb_create(count_free);
    rbtree_t *b = rb_create(count_free);
    CHECK(a != NULL && b != NULL, "rb_create returned NULL");
    if (!a || !b)
        goto done;

    int *va = mkval(1);
    if (!va)
        goto done;
    if (insert_owned(a, "shared", va) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        goto done;
    }

    CHECK(rb_find(a, "shared") == va, "key must resolve in the tree it went into");
    CHECK(rb_find(b, "shared") == NULL, "a second tree must not see the first tree's key");
    CHECK(rb_size(a) == 1, "inserted-into tree must have size 1");
    CHECK(rb_size(b) == 0, "untouched tree must have size 0");

done:
    rb_destroy(a);
    rb_destroy(b);
}

static void test_destroy_frees_owned_values(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    /* invariant: on leaving the loop, i values are owned by the tree */
    for (int i = 0; i < 3; ++i) {
        char key[KEY_BUF];
        make_key(key, sizeof key, i);
        int *v = mkval(i);
        if (!v)
            break;
        if (insert_owned(t, key, v) != 0) {
            CHECK(0, "rb_insert failed on a path that must succeed");
            break;
        }
    }

    size_t held = rb_size(t);
    rb_destroy(t);
    CHECK(g_free_calls == held, "rb_destroy must free each owned value exactly once");
}

static void test_destroy_leaves_unowned_values(void) {
    rbtree_t *t = rb_create(NULL); /* value_free NULL => values not owned */
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int *kept[3] = {0};
    /* invariant: kept[0..i) are live and reachable from the tree */
    for (int i = 0; i < 3; ++i) {
        char key[KEY_BUF];
        make_key(key, sizeof key, i);
        kept[i] = mkval(i);
        if (!kept[i])
            break;
        if (insert_owned(t, key, kept[i]) != 0) {
            CHECK(0, "rb_insert failed on a path that must succeed");
            kept[i] = NULL; /* insert_owned already released it */
            break;
        }
    }

    rb_destroy(t);

    /* If rb_destroy wrongly freed these, the frees below are double frees and
     * ASan/Valgrind report them. */
    for (int i = 0; i < 3; ++i) {
        if (kept[i])
            CHECK(*kept[i] == i, "unowned value must survive rb_destroy intact");
        free(kept[i]);
    }
}

/* ---- B. rb_insert, happy path ----------------------------------------- */

static void test_insert_single_key(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int *v = mkval(42);
    if (!v)
        goto done;

    int rc = insert_owned(t, "solo", v);
    CHECK(rc == 0, "rb_insert must return 0 on success");
    if (rc != 0)
        goto done;

    CHECK(rb_size(t) == 1, "size must be 1 after one insert");
    CHECK(rb_find(t, "solo") == v, "rb_find must return the exact value pointer");

done:
    rb_destroy(t);
}

static void test_insert_increments_size(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    /* invariant: after step i the tree holds exactly i+1 distinct keys */
    for (int i = 0; i < 16; ++i) {
        char key[KEY_BUF];
        make_key(key, sizeof key, i);
        int *v = mkval(i);
        if (!v)
            break;
        if (insert_owned(t, key, v) != 0) {
            CHECK(0, "rb_insert failed on a fresh key");
            break;
        }
        if (rb_size(t) != (size_t)(i + 1)) {
            CHECK(0, "each insert of a fresh key must grow size by exactly one");
            break;
        }
    }

    rb_destroy(t);
}

/* Bulk insertion orders. M0 has no fixup, so ascending and descending runs
 * build fully degenerate spines; the cap keeps that depth well inside the
 * stack, since an M0 rb_destroy may still recurse (O(1) space is M3). */
#define BULK_N 500

enum insert_order { ORDER_ASCENDING, ORDER_DESCENDING, ORDER_SHUFFLED };

static unsigned g_rng;

static unsigned rng_next(void) {
    g_rng = g_rng * 1103515245u + 12345u;
    return g_rng >> 16;
}

static void run_bulk_order(enum insert_order order) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int idx[BULK_N];
    for (int i = 0; i < BULK_N; ++i)
        idx[i] = (order == ORDER_DESCENDING) ? BULK_N - 1 - i : i;

    if (order == ORDER_SHUFFLED) {
        g_rng = 20260813u; /* fixed seed: any failure reproduces exactly */
        /* invariant: idx[i+1..] is already shuffled and untouched below */
        for (int i = BULK_N - 1; i > 0; --i) {
            int j      = (int)(rng_next() % (unsigned)(i + 1));
            int swap   = idx[i];
            idx[i]     = idx[j];
            idx[j]     = swap;
        }
    }

    /* invariant: every key drawn from idx[0..i) is present with its own value */
    for (int i = 0; i < BULK_N; ++i) {
        char key[KEY_BUF];
        make_key(key, sizeof key, idx[i]);
        int *v = mkval(idx[i]);
        if (!v)
            goto done;
        if (insert_owned(t, key, v) != 0) {
            CHECK(0, "rb_insert failed on a fresh key");
            goto done;
        }
        if (rb_size(t) != (size_t)(i + 1)) {
            CHECK(0, "bulk insert must grow size by exactly one per fresh key");
            goto done;
        }
    }

    for (int i = 0; i < BULK_N; ++i) {
        char key[KEY_BUF];
        make_key(key, sizeof key, i);
        const int *v = rb_find(t, key);
        CHECK(v != NULL && *v == i, "every bulk-inserted key must resolve to its own value");
    }

done:
    rb_destroy(t);
}

static void test_insert_ascending(void) {
    run_bulk_order(ORDER_ASCENDING);
}

static void test_insert_descending(void) {
    run_bulk_order(ORDER_DESCENDING);
}

static void test_insert_shuffled(void) {
    run_bulk_order(ORDER_SHUFFLED);
}

static void test_overwrite_replaces_value(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int *first = mkval(1);
    if (!first)
        goto done;
    if (insert_owned(t, "dup", first) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        goto done;
    }

    int *second = mkval(2);
    if (!second)
        goto done;
    int rc = insert_owned(t, "dup", second);
    CHECK(rc == 0, "overwriting an existing key must return 0");
    if (rc != 0)
        goto done;

    CHECK(rb_size(t) == 1, "overwriting a key must not grow the tree");
    CHECK(rb_find(t, "dup") == second, "rb_find must return the newest value");
    CHECK(g_free_calls == 1, "overwrite must free exactly one old value");
    CHECK(g_freed_n == 1 && g_freed[0] == first,
          "overwrite must free the displaced value, not the new one");

done:
    rb_destroy(t);
}

static void test_overwrite_without_value_free(void) {
    rbtree_t *t = rb_create(NULL); /* values not owned */
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int *first  = mkval(1);
    int *second = NULL;
    if (!first)
        goto done;
    if (insert_owned(t, "dup", first) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        first = NULL;
        goto done;
    }

    second = mkval(2);
    if (!second)
        goto done;
    if (insert_owned(t, "dup", second) != 0) {
        CHECK(0, "overwriting an existing key must succeed");
        second = NULL;
        goto done;
    }

    CHECK(rb_size(t) == 1, "overwriting a key must not grow the tree");
    CHECK(rb_find(t, "dup") == second, "rb_find must return the newest value");
    /* The displaced value is still ours: reading it here is a use-after-free
     * report if the tree released it despite value_free being NULL. */
    CHECK(*first == 1, "with no value_free, the displaced value must not be freed");

done:
    rb_destroy(t);
    free(first);
    free(second);
}

static void test_insert_copies_key(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    char *caller_key = strdup("borrowed");
    CHECK(caller_key != NULL, "test harness: strdup failed");
    if (!caller_key)
        goto done;

    int *v = mkval(7);
    if (!v) {
        free(caller_key);
        goto done;
    }
    if (insert_owned(t, caller_key, v) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        free(caller_key);
        goto done;
    }

    /* Release the caller's buffer. If the tree aliased it instead of copying,
     * the lookup below reads freed memory and ASan/Valgrind say so. */
    memset(caller_key, 'x', strlen(caller_key));
    free(caller_key);

    CHECK(rb_find(t, "borrowed") == v, "the tree must own a copy of the key");

done:
    rb_destroy(t);
}

static void test_key_edge_cases(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    char *long_key = malloc(4096);
    CHECK(long_key != NULL, "test harness: malloc for a long key failed");
    if (!long_key)
        goto done;
    memset(long_key, 'L', 4095);
    long_key[4095] = '\0';

    static const char *const fixed[] = {"", "a"};
    /* invariant: fixed[0..i) are present with value i */
    for (int i = 0; i < 2; ++i) {
        int *v = mkval(i);
        if (!v)
            goto cleanup_key;
        if (insert_owned(t, fixed[i], v) != 0) {
            CHECK(0, "rb_insert failed on an edge-case key");
            goto cleanup_key;
        }
    }

    int *lv = mkval(2);
    if (!lv)
        goto cleanup_key;
    if (insert_owned(t, long_key, lv) != 0) {
        CHECK(0, "rb_insert failed on a 4 KB key");
        goto cleanup_key;
    }

    CHECK(rb_size(t) == 3, "three distinct keys must yield size 3");

    const int *ev = rb_find(t, "");
    CHECK(ev != NULL && *ev == 0, "the empty key must be storable and findable");
    const int *av = rb_find(t, "a");
    CHECK(av != NULL && *av == 1, "a single-character key must be findable");
    CHECK(rb_find(t, long_key) == lv, "a 4 KB key must be findable");

cleanup_key:
    free(long_key);
done:
    rb_destroy(t);
}

static void test_keys_sharing_a_prefix(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    /* Differ only in the final byte, past a long common run: catches a
     * comparison that stops short or compares a fixed number of bytes. */
    static const char *const keys[] = {
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    };
    const int n = (int)(sizeof keys / sizeof keys[0]);

    /* invariant: keys[0..i) are present, each mapped to its own index */
    for (int i = 0; i < n; ++i) {
        int *v = mkval(i);
        if (!v)
            goto done;
        if (insert_owned(t, keys[i], v) != 0) {
            CHECK(0, "rb_insert failed on a prefix-sharing key");
            goto done;
        }
    }

    CHECK(rb_size(t) == (size_t)n, "prefix-sharing keys must all be distinct");
    for (int i = 0; i < n; ++i) {
        const int *v = rb_find(t, keys[i]);
        CHECK(v != NULL && *v == i, "each prefix-sharing key must resolve to its own value");
    }

done:
    rb_destroy(t);
}

static void test_high_bit_keys(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    /* Bytes straddling the signed-char boundary. Any self-consistent ordering
     * still gives a working BST, so this does not catch a uniformly signed
     * comparator; it catches mixing one with strcmp's unsigned semantics, and
     * any comparator that mistakes 0x80 for a terminator. */
    static const char k01[] = {(char)0x01, '\0'};
    static const char k7f[] = {(char)0x7f, '\0'};
    static const char k80[] = {(char)0x80, '\0'};
    static const char kff[] = {(char)0xff, '\0'};
    const char *const  keys[] = {k01, k7f, k80, kff};
    const int          n      = (int)(sizeof keys / sizeof keys[0]);

    /* invariant: keys[0..i) are present, each mapped to its own index */
    for (int i = 0; i < n; ++i) {
        int *v = mkval(i);
        if (!v)
            goto done;
        if (insert_owned(t, keys[i], v) != 0) {
            CHECK(0, "rb_insert failed on a high-bit key");
            goto done;
        }
    }

    CHECK(rb_size(t) == (size_t)n, "high-bit keys must all be distinct");
    for (int i = 0; i < n; ++i) {
        const int *v = rb_find(t, keys[i]);
        CHECK(v != NULL && *v == i, "each high-bit key must resolve to its own value");
    }

done:
    rb_destroy(t);
}

/* ---- C. rb_find -------------------------------------------------------- */

static void test_find_on_empty_tree(void) {
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    CHECK(rb_find(t, "anything") == NULL, "lookup in an empty tree must return NULL");
    CHECK(rb_find(t, "") == NULL, "empty-key lookup in an empty tree must return NULL");
    CHECK(rb_size(t) == 0, "a failed lookup must not create nodes");

    rb_destroy(t);
}

static void test_find_absent_keys(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    static const char *const present[] = {"ab", "abc", "m"};
    /* invariant: present[0..i) are in the tree */
    for (int i = 0; i < 3; ++i) {
        int *v = mkval(i);
        if (!v)
            goto done;
        if (insert_owned(t, present[i], v) != 0) {
            CHECK(0, "rb_insert failed on a path that must succeed");
            goto done;
        }
    }

    CHECK(rb_find(t, "a") == NULL, "a strict prefix of a present key must be absent");
    CHECK(rb_find(t, "abcd") == NULL, "a strict extension of a present key must be absent");
    CHECK(rb_find(t, "") == NULL, "the empty key was never inserted");
    CHECK(rb_find(t, "zzz") == NULL, "a key ordered after every node must be absent");
    CHECK(rb_find(t, "A") == NULL, "a key ordered before every node must be absent");
    CHECK(rb_size(t) == 3, "failed lookups must not change size");

done:
    rb_destroy(t);
}

static void test_find_is_stable_and_const(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int *v = mkval(9);
    if (!v)
        goto done;
    if (insert_owned(t, "stable", v) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        goto done;
    }

    /* Exercises the const-qualified parameter in the header contract. */
    const rbtree_t *ct = t;
    for (int i = 0; i < 3; ++i)
        CHECK(rb_find(ct, "stable") == v, "repeated lookups must return the same value");
    CHECK(rb_size(ct) == 1, "lookups must not change size");

done:
    rb_destroy(t);
}

/* ---- runner ------------------------------------------------------------ */

struct test_case {
    const char *name;
    void (*fn)(void);
};

static const struct test_case k_tests[] = {
    {"create_returns_empty_tree", test_create_returns_empty_tree},
    {"create_with_value_free", test_create_with_value_free},
    {"destroy_null_is_safe", test_destroy_null_is_safe},
    {"trees_are_independent", test_trees_are_independent},
    {"destroy_frees_owned_values", test_destroy_frees_owned_values},
    {"destroy_leaves_unowned_values", test_destroy_leaves_unowned_values},
    {"insert_single_key", test_insert_single_key},
    {"insert_increments_size", test_insert_increments_size},
    {"insert_ascending", test_insert_ascending},
    {"insert_descending", test_insert_descending},
    {"insert_shuffled", test_insert_shuffled},
    {"overwrite_replaces_value", test_overwrite_replaces_value},
    {"overwrite_without_value_free", test_overwrite_without_value_free},
    {"insert_copies_key", test_insert_copies_key},
    {"key_edge_cases", test_key_edge_cases},
    {"keys_sharing_a_prefix", test_keys_sharing_a_prefix},
    {"high_bit_keys", test_high_bit_keys},
    {"find_on_empty_tree", test_find_on_empty_tree},
    {"find_absent_keys", test_find_absent_keys},
    {"find_is_stable_and_const", test_find_is_stable_and_const},
};

int main(void) {
    const size_t n = sizeof k_tests / sizeof k_tests[0];

    /* Failures go to stderr and progress to stdout; unbuffering keeps the two
     * in true order when the run is piped to a log or through Valgrind. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* invariant: g_failures counts every failed CHECK from tests 0..i */
    for (size_t i = 0; i < n; ++i) {
        int before      = g_failures;
        g_current_test  = k_tests[i].name;
        reset_free_log();
        k_tests[i].fn();
        if (g_failures == before)
            printf("ok   %s\n", k_tests[i].name);
    }
    g_current_test = "<none>";

    if (g_failures != 0) {
        fprintf(stderr, "\n%d check(s) failed across %zu test(s)\n", g_failures, n);
        return 1;
    }
    printf("\nall %zu test(s) passed\n", n);
    return 0;
}
