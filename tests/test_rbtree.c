/* Unit tests: rb_create, rb_insert (red-black, with fixup), rb_find,
 * rb_destroy, with rb_size as a node-count oracle.
 *
 * Every assertion here is on behavior observable through the public API.
 * Shape and balance are asserted only through rb_validate, which reports
 * whether the red-black invariants hold: struct rb_node is private to
 * src/rbtree.c, so no assertion here depends on a particular tree layout. */

/* Source-relative path: the build links every .c in one gcc invocation and
 * resolves -Iinclude against make's CWD, so do not depend on the search path. */
#include "../include/rbtree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- harness ---------------------------------------------------------- */

static int         g_failures;
static const char *g_current_test = "<none>";
/* Names the table row a failure came from, or NULL outside a table-driven run. */
static const char *g_current_case;

static void report_failure(int line, const char *msg) {
    ++g_failures;
    if (g_current_case)
        fprintf(stderr, "FAIL %s[%s] (line %d): %s\n", g_current_test, g_current_case, line,
                msg);
    else
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

/* Bulk insertion orders. Ascending and descending runs are the orders a
 * missing or broken fixup degenerates on, so each one validates after every
 * single insert rather than only at the end: that pins down the insert that
 * first broke an invariant instead of reporting the damage 500 keys later. */
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
        if (rb_validate(t) != 0) {
            CHECK(0, "red-black invariants must hold after every insert");
            goto done;
        }
    }

    CHECK(rb_validate(t) == 0, "invariants must hold after a full bulk run");

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
    /* An overwrite links no node, so it must leave coloring untouched too. */
    CHECK(rb_validate(t) == 0, "overwriting a key must leave invariants intact");
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
    CHECK(rb_validate(t) == 0, "overwriting a key must leave invariants intact");
    /* The displaced value is still ours: reading it here is a use-after-free
     * report if the tree released it despite value_free being NULL. */
    CHECK(*first == 1, "with no value_free, the displaced value must not be freed");

done:
    rb_destroy(t);
    free(first);
    free(second);
}

/* Re-inserting the pointer the tree already holds, under its own key. rb_insert
 * takes the overwrite path, and the value it would free is the value it is about
 * to store: freeing it would leave the tree holding a dangling pointer that the
 * very next rb_find hands back. Nothing else in the suite reaches that branch --
 * every other test, and the fuzzer, supplies a freshly allocated value, which can
 * never equal a pointer the tree is still holding. */
static void test_reinsert_same_value_frees_nothing(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int *v = mkval(7);
    if (!v)
        goto done;
    if (insert_owned(t, "dup", v) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        goto done;
    }

    /* Deliberately not insert_owned: the tree already owns v, so releasing it on
     * a reported failure would be a double free rather than a cleanup. */
    int rc = rb_insert(t, "dup", v);
    CHECK(rc == 0, "re-inserting the stored value under its own key must return 0");
    CHECK(g_free_calls == 0, "re-inserting the stored value must free nothing");

    /* Reading through v is the assertion that matters: if the tree released it,
     * this is a use-after-free for ASan and Valgrind to report. */
    CHECK(*v == 7, "the re-inserted value must still be readable");
    CHECK(rb_find(t, "dup") == v, "rb_find must still return the re-inserted value");
    CHECK(rb_size(t) == 1, "re-inserting a stored value must not grow the tree");
    CHECK(rb_validate(t) == 0, "re-inserting a stored value must leave invariants intact");

done:
    rb_destroy(t);
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

/* ---- D. rb_validate ---------------------------------------------------- */

/* rb_validate is the only window onto tree shape the public API offers, so the
 * tests above lean on it heavily; these pin down its own edge cases first. */

static void test_validate_empty_tree(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    CHECK(rb_validate(t) == 0, "an empty tree must satisfy the invariants");

    int *v = mkval(1);
    if (!v)
        goto done;
    if (insert_owned(t, "only", v) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        goto done;
    }
    /* A one-node tree is the smallest case that can get the root's color
     * wrong: the node goes in red and must be recolored black. */
    CHECK(rb_validate(t) == 0, "a single-node tree must satisfy the invariants");

done:
    rb_destroy(t);
}

/* Keys chosen so ordering decisions inside the fixup have to agree with the
 * strcmp ordering the descent uses: an empty key that is a prefix of every
 * other, keys differing only past a long common run, and bytes on both sides
 * of the signed-char boundary. */
static void test_validate_after_edge_keys(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    static const char k80[] = {(char)0x80, '\0'};
    static const char kff[] = {(char)0xff, '\0'};
    static const char k01[] = {(char)0x01, '\0'};

    const char *const keys[] = {
        "", "a", k80, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", kff,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", k01,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ab", "b",
    };
    const int n = (int)(sizeof keys / sizeof keys[0]);

    /* invariant: keys[0..i) are present and the tree is a valid red-black
     * tree after each one of them individually */
    for (int i = 0; i < n; ++i) {
        int *v = mkval(i);
        if (!v)
            goto done;
        if (insert_owned(t, keys[i], v) != 0) {
            CHECK(0, "rb_insert failed on an edge-case key");
            goto done;
        }
        if (rb_validate(t) != 0) {
            CHECK(0, "invariants must hold after each edge-case key");
            goto done;
        }
    }

    CHECK(rb_size(t) == (size_t)n, "edge-case keys must all be distinct");
    for (int i = 0; i < n; ++i) {
        const int *v = rb_find(t, keys[i]);
        CHECK(v != NULL && *v == i, "each edge-case key must resolve to its own value");
    }

done:
    rb_destroy(t);
}

/* ---- E. rb_delete ------------------------------------------------------ */

/* struct rb_node is private to src/rbtree.c, so nothing here can read a node's
 * color. Each case names an insert order instead, and the comment above it
 * draws the tree that order produces, so the configuration rb_delete faces is
 * on the record even though the test cannot see it. Every diagram was taken
 * from the real insert path rather than derived by hand; a child that is not
 * drawn is a NULL (black) leaf.
 *
 * "near" and "far" name the deleted node's nephews relative to itself: for a
 * left child the near nephew is the sibling's left child, mirrored for a right
 * child. Both orientations of every configuration appear, because a fixup that
 * handles one side and mirrors it wrongly still passes the one-sided half. */

struct delete_case {
    const char        *name;
    const char *const *inserts; /* NULL-terminated, in insert order */
    const char        *victim;
    int                expect_rc;
};

/*      b(B)
 *     /    \
 *   a(R)   c(R)          Red leaves on both sides, and the smallest root with
 *                        two children, whose successor is its own right child.
 */
static const char *const dc_three_node[] = {"b", "a", "c", NULL};

/*        d(B)
 *       /    \
 *    b(B)    f(R)        the sixth key recolors f red through the red-uncle
 *           /    \       branch of the insert fixup, which is the only way an
 *        e(B)    g(B)    insert-only sequence reaches a red sibling.
 *            \
 *           ee(R)
 */
static const char *const dc_red_sibling_right[] = {"d", "b", "f", "e", "g", "ee", NULL};

/*          d(B)
 *         /    \
 *      b(R)    f(B)      mirror of dc_red_sibling_right.
 *     /    \
 *  a(B)    c(B)
 *      \
 *     aa(R)
 */
static const char *const dc_red_sibling_left[] = {"d", "f", "b", "c", "a", "aa", NULL};

/* m(B) alone: deleting it must empty the tree, not merely unlink it. */
static const char *const dc_sole_node[] = {"m", NULL};

/*    b(B)
 *   /                    root with one child; the child must become the new
 * a(R)                   black root.
 */
static const char *const dc_root_one_child_left[] = {"b", "a", NULL};

/* a(B)
 *     \                  mirror of dc_root_one_child_left.
 *     b(R)
 */
static const char *const dc_root_one_child_right[] = {"a", "b", NULL};

/*          d(B)
 *        /      \
 *     b(B)      f(B)     every internal node has two children, so this one tree
 *    /    \    /    \    covers a left child, a right child, and a root whose
 * a(R)  c(R) e(R)  g(R)  successor is a grandchild rather than its right child.
 */
static const char *const dc_perfect[] = {"d", "b", "f", "a", "c", "e", "g", NULL};

/*    d(B)
 *   /    \                black sibling with a red far nephew and a nil near
 * b(B)   f(B)             nephew: the rotate-once case, and the near nephew is
 *            \            black so an implementation that tests the wrong
 *            g(R)         nephew cannot pass by accident.
 */
static const char *const dc_far_nephew_right[] = {"d", "b", "f", "g", NULL};

/*      d(B)
 *     /    \              mirror of dc_far_nephew_right.
 *  b(B)    f(B)
 *  /
 * a(R)
 */
static const char *const dc_far_nephew_left[] = {"d", "f", "b", "a", NULL};

/*    d(B)
 *   /    \                black sibling with a red near nephew and a nil far
 * b(B)   f(B)             nephew: the case that must rotate the sibling first
 *        /                and only then rotate the parent.
 *      e(R)
 */
static const char *const dc_near_nephew_right[] = {"d", "b", "f", "e", NULL};

/*      d(B)
 *     /    \              mirror of dc_near_nephew_right.
 *  b(B)    f(B)
 *      \
 *      c(R)
 */
static const char *const dc_near_nephew_left[] = {"d", "f", "b", "c", NULL};

/*            d(B)
 *          /      \
 *       b(R)      f(R)        a and c are black leaves that are each other's
 *      /    \    /    \       sibling, so both nephews are nil: the recoloring
 *   a(B)  c(B) e(B)  g(B)     case. Their parent b is red, so the fixup absorbs
 *                       \     the extra black there and stops immediately.
 *                       h(R)
 */
static const char *const dc_black_nephews_red_parent[] = {"a", "b", "c", "d", "e",
                                                          "f", "g", "h", NULL};

/*            d(B)
 *          /      \
 *       b(B)      f(B)        same recoloring case, but b is black, so the
 *      /    \    /    \       extra black cannot stop there and must climb to
 *   a(B)  c(B) e(B)  h(R)     b, where the sibling f is black with a red far
 *                  /    \     nephew. One delete therefore runs two fixup
 *               g(B)    i(B)  iterations.
 *                          \
 *                          j(R)
 */
static const char *const dc_black_nephews_climb[] = {"a", "b", "c", "d", "e",
                                                     "f", "g", "h", "i", "j", NULL};

/* No keys at all: rb_delete must report absence rather than dereference a NULL
 * root. */
static const char *const dc_empty[] = {NULL};

/* Keys where one is a strict prefix of another, so an absent key can still walk
 * a full root-to-leaf path before failing. */
static const char *const dc_prefix[] = {"ab", "abc", "m", NULL};

static const struct delete_case k_delete_cases[] = {
    /* red leaf */
    {"red_leaf_left", dc_three_node, "a", 0},
    {"red_leaf_right", dc_three_node, "c", 0},

    /* black leaf whose sibling is red */
    {"red_sibling_right", dc_red_sibling_right, "b", 0},
    {"red_sibling_left", dc_red_sibling_left, "f", 0},

    /* the root itself */
    {"root_sole_node", dc_sole_node, "m", 0},
    {"root_one_child_left", dc_root_one_child_left, "b", 0},
    {"root_one_child_right", dc_root_one_child_right, "a", 0},
    {"root_two_children", dc_three_node, "b", 0},

    /* both children present */
    {"two_children_left_child", dc_perfect, "b", 0},
    {"two_children_right_child", dc_perfect, "f", 0},
    {"two_children_deeper_successor", dc_perfect, "d", 0},

    /* black leaf whose sibling is black */
    {"black_sibling_far_nephew_right", dc_far_nephew_right, "b", 0},
    {"black_sibling_far_nephew_left", dc_far_nephew_left, "f", 0},
    {"black_sibling_near_nephew_right", dc_near_nephew_right, "b", 0},
    {"black_sibling_near_nephew_left", dc_near_nephew_left, "f", 0},
    {"black_nephews_red_parent_left", dc_black_nephews_red_parent, "a", 0},
    {"black_nephews_red_parent_right", dc_black_nephews_red_parent, "c", 0},
    {"black_nephews_climb_left", dc_black_nephews_climb, "a", 0},
    {"black_nephews_climb_right", dc_black_nephews_climb, "c", 0},

    /* absent keys: the tree must come through untouched */
    {"absent_from_empty_tree", dc_empty, "anything", -1},
    {"absent_key_after_every_node", dc_perfect, "zzz", -1},
    {"absent_key_is_a_prefix", dc_prefix, "a", -1},
};

/* Builds the case's tree, deletes one key, and checks every post-condition the
 * header fixes: the return code, the size, the victim's absence, which value was
 * released, the red-black invariants, and the survival of every other key. */
static void run_delete_case(const struct delete_case *c) {
    int before = g_failures;

    g_current_case = c->name;
    reset_free_log();

    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        goto done;

    /* invariant: inserts[0..i) are present, each mapped to its own index, and
     * the tree is a valid red-black tree after every one of them; a failure
     * here is a broken case setup, not a defect in rb_delete */
    int i = 0;
    for (; c->inserts[i]; ++i) {
        int *v = mkval(i);
        if (!v)
            goto cleanup;
        if (insert_owned(t, c->inserts[i], v) != 0) {
            CHECK(0, "case setup: rb_insert failed on a fresh key");
            goto cleanup;
        }
        if (rb_validate(t) != 0) {
            CHECK(0, "case setup: the insert order must build a valid tree");
            goto cleanup;
        }
    }
    const int n = i;
    if (rb_size(t) != (size_t)n) {
        CHECK(0, "case setup: the keys of a case must all be distinct");
        goto cleanup;
    }

    void *victim_value = rb_find(t, c->victim);
    CHECK((victim_value != NULL) == (c->expect_rc == 0),
          "case setup: a case must expect 0 exactly when its victim is present");

    int rc = rb_delete(t, c->victim);
    CHECK(rc == c->expect_rc, "rb_delete returned the wrong code");

    if (c->expect_rc == 0) {
        CHECK(rb_size(t) == (size_t)n - 1, "a successful delete must shrink size by exactly one");
        CHECK(rb_find(t, c->victim) == NULL, "a deleted key must no longer be findable");
        CHECK(g_free_calls == 1, "a successful delete must free exactly one value");
        CHECK(g_freed_n == 1 && g_freed[0] == victim_value,
              "a successful delete must free the deleted key's value and no other");
    } else {
        CHECK(rb_size(t) == (size_t)n, "a failed delete must leave size unchanged");
        CHECK(g_free_calls == 0, "a failed delete must free no value");
    }

    CHECK(rb_validate(t) == 0, "the invariants must hold after rb_delete");

    /* invariant: every key but the victim still resolves to the value it was
     * inserted with, which catches a delete that reorders the tree or moves a
     * value onto the wrong key -- neither of which rb_validate can see */
    for (i = 0; i < n; ++i) {
        if (c->expect_rc == 0 && strcmp(c->inserts[i], c->victim) == 0)
            continue;
        const int *v = rb_find(t, c->inserts[i]);
        if (v == NULL || *v != i) {
            CHECK(0, "every surviving key must still resolve to its own value");
            break;
        }
    }

cleanup:
    rb_destroy(t);
done:
    if (g_failures == before)
        printf("ok   delete/%s\n", c->name);
    g_current_case = NULL;
}

static void test_delete_table(void) {
    const size_t n = sizeof k_delete_cases / sizeof k_delete_cases[0];
    /* invariant: cases 0..i have each run against their own fresh tree */
    for (size_t i = 0; i < n; ++i)
        run_delete_case(&k_delete_cases[i]);
}

/* The header gives rb_delete the value as well as the key copy, but only when
 * the tree owns values at all. */
static void test_delete_without_value_free(void) {
    rbtree_t *t = rb_create(NULL); /* value_free NULL => values not owned */
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int *v = mkval(5);
    if (!v)
        goto done;
    if (insert_owned(t, "solo", v) != 0) {
        CHECK(0, "rb_insert failed on a path that must succeed");
        v = NULL;
        goto done;
    }

    CHECK(rb_delete(t, "solo") == 0, "deleting a present key must return 0");
    CHECK(rb_size(t) == 0, "deleting the only key must empty the tree");
    CHECK(rb_validate(t) == 0, "an emptied tree must satisfy the invariants");
    /* The value is still ours: reading it here is a use-after-free report, and
     * the free below a double free, if rb_delete released what it does not own. */
    CHECK(*v == 5, "with no value_free, a deleted value must survive");

done:
    rb_destroy(t);
    free(v);
}

/* A delete that leaves the key copy reachable, or a stale parent pointer behind,
 * shows up on the next insert rather than at the delete itself. */
static void test_delete_then_reinsert(void) {
    rbtree_t *t = rb_create(count_free);
    CHECK(t != NULL, "rb_create returned NULL");
    if (!t)
        return;

    int i = 0;
    /* invariant: dc_perfect[0..i) are present, each mapped to its own index */
    for (; dc_perfect[i]; ++i) {
        int *v = mkval(i);
        if (!v)
            goto done;
        if (insert_owned(t, dc_perfect[i], v) != 0) {
            CHECK(0, "rb_insert failed on a fresh key");
            goto done;
        }
    }
    const int n = i;

    CHECK(rb_delete(t, "b") == 0, "deleting a present key must return 0");
    CHECK(rb_size(t) == (size_t)n - 1, "a successful delete must shrink size by one");
    CHECK(rb_find(t, "b") == NULL, "a deleted key must no longer be findable");
    CHECK(rb_validate(t) == 0, "the invariants must hold after rb_delete");

    int *again = mkval(99);
    if (!again)
        goto done;
    if (insert_owned(t, "b", again) != 0) {
        CHECK(0, "reinserting a deleted key must succeed");
        goto done;
    }

    CHECK(rb_size(t) == (size_t)n, "reinserting a deleted key must restore the size");
    CHECK(rb_find(t, "b") == again, "a reinserted key must resolve to its new value");
    CHECK(rb_validate(t) == 0, "the invariants must hold after a delete and reinsert");
    /* One value was displaced by the delete and none by the reinsert, since the
     * key was genuinely gone rather than merely unreachable. */
    CHECK(g_free_calls == 1, "delete then reinsert must free exactly the deleted value");

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
    {"reinsert_same_value_frees_nothing", test_reinsert_same_value_frees_nothing},
    {"insert_copies_key", test_insert_copies_key},
    {"key_edge_cases", test_key_edge_cases},
    {"keys_sharing_a_prefix", test_keys_sharing_a_prefix},
    {"high_bit_keys", test_high_bit_keys},
    {"find_on_empty_tree", test_find_on_empty_tree},
    {"find_absent_keys", test_find_absent_keys},
    {"find_is_stable_and_const", test_find_is_stable_and_const},
    {"validate_empty_tree", test_validate_empty_tree},
    {"validate_after_edge_keys", test_validate_after_edge_keys},
    {"delete_table", test_delete_table},
    {"delete_without_value_free", test_delete_without_value_free},
    {"delete_then_reinsert", test_delete_then_reinsert},
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
