/* Differential fuzzer: a long randomized stream of rb_insert / rb_find /
 * rb_delete, mirrored into a reference list that is correct by construction,
 * with the tree checked against that model after every single operation.
 *
 * The unit suite covers named shapes and edge cases; this covers the sequences
 * nobody thought to name, where rebalancing bugs, size drift, and ownership
 * mistakes (double free, use-after-free, leaked key copies) actually hide.
 *
 * The run is deterministic: the seed is a fixed constant, not an argument, so
 * any failure reproduces exactly from the line it prints. On the first
 * divergence the process dies. A fuzzer is one dependent sequence, not a table
 * of independent cases: past the first divergence the model and the tree
 * disagree by construction, and every later complaint only buries the one that
 * mattered.
 *
 * The oracle is rb_find, rb_size, and rb_validate. In-order traversal is
 * deliberately not cross-checked: rb_foreach is still an M0 stub that visits
 * nothing, so a traversal check would report an empty tree on every call. Add
 * one here in the milestone that implements it. */

/* Source-relative path: the build links every .c in one gcc invocation and
 * resolves -Iinclude against make's CWD, so do not depend on the search path. */
#include "../include/rbtree.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

/* ---- parameters ------------------------------------------------------- */

/* Keys are drawn from a small dense universe on purpose: inserts then land on
 * an existing key often enough to exercise the overwrite path, deletes mostly
 * hit, and the model's linear scan stays bounded. */
#define KEY_UNIVERSE   1024
#define KEY_BUF        16
#define VALIDATE_EVERY 100

#define DEFAULT_ITERS 100000UL
#define DEFAULT_SEED  20260819U /* fixed seed: any failure reproduces exactly */

/* ---- failure reporting ------------------------------------------------ */

/* Enough state to make a failure line self-contained: what was being done, to
 * which key, and how far in. The seed is a constant, so it needs no global. */
static unsigned long g_iters = DEFAULT_ITERS;
static unsigned long g_iter;
static const char   *g_op  = "<setup>";
static const char   *g_key = "<none>";

[[noreturn, gnu::format(printf, 2, 3)]]
static void fail(int line, const char *fmt, ...) {
    fprintf(stderr, "FAIL fuzz.c:%d iter=%lu op=%s key=%s\n  ", line, g_iter, g_op, g_key);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n  reproduce: ./build/fuzz %lu\n", g_iters);
    exit(1);
}

#define CHECK(cond, ...)                 \
    do {                                 \
        if (!(cond))                     \
            fail(__LINE__, __VA_ARGS__); \
    } while (0)

/* ---- rng -------------------------------------------------------------- */

/* xorshift32 rather than the unit suite's LCG: that one publishes only the top
 * bits of its state, and a key index taken modulo 1024 from an LCG's low bits
 * cycles far too short to be a fair operation stream. */
static uint32_t g_rng_state;

static uint32_t rng_next(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

/* Unpadded on purpose. Zero-padding would make every key exactly five bytes, so
 * no key would ever be a prefix of another and the key copy would be exercised
 * at one length only; ids 1, 12, 123 and 1023 instead give a 2-to-5 byte chain
 * where each key is a prefix of the next, which is the ordering case node_find
 * treats as an ordinary neighbour rather than a special case. */
static void make_key(char *buf, size_t n, unsigned id) {
    snprintf(buf, n, "k%u", id);
}

/* ---- reference model -------------------------------------------------- */

/* Test-side allocations use plain malloc/free, never rb_malloc: the
 * interception point in tests/fault_alloc.h is scoped to allocations made by
 * src/, and keeping test allocations out of it leaves its counters free to
 * measure the library alone once fault injection is added. */

/* value is an observation, not an owning reference: the tree was created with a
 * value_free, so it releases values on overwrite, delete, and destroy. payload
 * is a copy of what the value pointed at when it was handed over, so a find can
 * check that the tree returned the right storage AND the right contents. */
struct ref {
    char  key[KEY_BUF];
    void *value;
    int   payload;
    TAILQ_ENTRY(ref) link;
};

TAILQ_HEAD(ref_head, ref);

static struct ref_head g_ref = TAILQ_HEAD_INITIALIZER(g_ref);
static size_t          g_ref_count;

static struct ref *ref_find(const char *key) {
    struct ref *e;
    /* invariant: the model holds at most one entry per key, so the first match
     * is the only match */
    TAILQ_FOREACH(e, &g_ref, link)
        if (strcmp(e->key, key) == 0)
            return e;
    return NULL;
}

/* found is ref_find's result for key, passed in so the caller's lookup is not
 * repeated. NULL means the key is new. */
static void ref_put(struct ref *found, const char *key, void *value, int payload) {
    if (found) {
        /* The tree freed the old value during the overwrite, so the model only
         * re-points; it never owned that storage. */
        found->value   = value;
        found->payload = payload;
        return;
    }

    struct ref *e = malloc(sizeof *e);
    if (!e)
        fail(__LINE__, "fuzzer out of memory allocating a model entry");

    snprintf(e->key, sizeof e->key, "%s", key);
    e->value   = value;
    e->payload = payload;
    TAILQ_INSERT_HEAD(&g_ref, e, link);
    ++g_ref_count;
}

static void ref_remove(struct ref *e) {
    TAILQ_REMOVE(&g_ref, e, link);
    free(e); /* the value belonged to the tree, which has already freed it */
    --g_ref_count;
}

/* glibc's <sys/queue.h> ships no TAILQ_FOREACH_SAFE at any feature-test level:
 * the _SAFE variants are a BSD extension it simply does not carry. Draining
 * from the head needs no lookahead, so none is missed. */
static void ref_clear(void) {
    /* invariant: every entry already unlinked has been freed, and the head
     * always points at the next one to release */
    while (!TAILQ_EMPTY(&g_ref)) {
        struct ref *e = TAILQ_FIRST(&g_ref);
        TAILQ_REMOVE(&g_ref, e, link);
        free(e);
    }
    g_ref_count = 0;
}

/* ---- operations ------------------------------------------------------- */

static unsigned long g_n_insert, g_n_overwrite, g_n_find_hit, g_n_find_miss;
static unsigned long g_n_delete_hit, g_n_delete_miss;

static void do_insert(rbtree_t *t, const char *key) {
    struct ref *e = ref_find(key);

    int *v = malloc(sizeof *v);
    if (!v)
        fail(__LINE__, "fuzzer out of memory allocating a value");
    /* The iteration number makes every value distinct, so a tree that hands
     * back a stale value is caught even when the key is right. */
    *v = (int)g_iter;

    int rc = rb_insert(t, key, v);
    if (rc != 0) {
        /* Only a real out-of-memory can land here today, rb_malloc being a
         * pass-through. The contract says the value was not consumed and the
         * tree is unchanged, so the value is ours to release and the model must
         * not move. */
        CHECK(rc == -1, "rb_insert returned %d, expected 0 or -1", rc);
        free(v);
        return;
    }

    if (e)
        ++g_n_overwrite;
    else
        ++g_n_insert;

    void *got = rb_find(t, key);
    CHECK(got == v, "rb_find after insert returned %p, expected the value just inserted %p",
          got, (void *)v);
    CHECK(*(int *)got == *v, "rb_find after insert returned payload %d, expected %d",
          *(int *)got, *v);

    ref_put(e, key, v, *v);
    CHECK(rb_size(t) == g_ref_count, "rb_size is %zu after insert, model holds %zu",
          rb_size(t), g_ref_count);
}

static void do_find(const rbtree_t *t, const char *key) {
    struct ref *e   = ref_find(key);
    void       *got = rb_find(t, key);

    if (!e) {
        ++g_n_find_miss;
        CHECK(got == NULL, "rb_find returned %p for a key the model does not hold", got);
        return;
    }

    ++g_n_find_hit;
    CHECK(got == e->value, "rb_find returned %p, model holds %p", got, e->value);
    CHECK(*(int *)got == e->payload, "rb_find returned payload %d, model holds %d",
          *(int *)got, e->payload);
}

static void do_delete(rbtree_t *t, const char *key) {
    struct ref *e  = ref_find(key);
    int         rc = rb_delete(t, key);

    if (!e) {
        ++g_n_delete_miss;
        CHECK(rc == -1, "rb_delete returned %d for an absent key, expected -1", rc);
        CHECK(rb_size(t) == g_ref_count, "rb_size is %zu after a failed delete, model holds %zu",
              rb_size(t), g_ref_count);
        return;
    }

    ++g_n_delete_hit;
    CHECK(rc == 0, "rb_delete returned %d for a present key, expected 0", rc);

    void *got = rb_find(t, key);
    CHECK(got == NULL, "rb_find returned %p for a key just deleted", got);

    ref_remove(e);
    CHECK(rb_size(t) == g_ref_count, "rb_size is %zu after delete, model holds %zu",
          rb_size(t), g_ref_count);
}

/* Everything the per-operation checks cannot see: the red-black invariants
 * themselves, and whether keys inserted long ago are all still reachable with
 * the storage they were given. */
static void deep_check(const rbtree_t *t) {
    CHECK(rb_validate(t) == 0, "rb_validate rejected the tree");
    CHECK(rb_size(t) == g_ref_count, "rb_size is %zu, model holds %zu", rb_size(t), g_ref_count);

    struct ref *e;
    /* invariant: every entry visited so far is present in the tree with the
     * exact value pointer and payload the model recorded for it */
    TAILQ_FOREACH(e, &g_ref, link) {
        void *got = rb_find(t, e->key);
        if (got != e->value)
            fail(__LINE__, "sweep: rb_find(%s) returned %p, model holds %p", e->key, got,
                 e->value);
        if (*(int *)got != e->payload)
            fail(__LINE__, "sweep: rb_find(%s) returned payload %d, model holds %d", e->key,
                 *(int *)got, e->payload);
    }
}

/* ---- driver ----------------------------------------------------------- */

[[noreturn]]
static void usage(const char *argv0, const char *why) {
    fprintf(stderr, "%s: %s\nusage: %s [iterations]   (default %lu)\n", argv0, why, argv0,
            DEFAULT_ITERS);
    /* Exit 2, distinct from fail()'s 1: a bad invocation is not a divergence,
     * and a harness must not read one as the other. */
    exit(2);
}

/* Rejects rather than falls back, because a silently ignored argument turns a
 * run that never happened into a pass. strtoul alone is too permissive here: it
 * skips leading whitespace and quietly wraps a negative, so the first character
 * is checked to be a digit before it is consulted at all. */
static unsigned long parse_iters(const char *argv0, const char *arg) {
    if (arg[0] < '0' || arg[0] > '9')
        usage(argv0, "iteration count must be a plain decimal number");

    errno             = 0;
    char         *end = NULL;
    unsigned long n   = strtoul(arg, &end, 10);

    if (*end != '\0')
        usage(argv0, "iteration count has trailing junk");
    if (errno == ERANGE)
        usage(argv0, "iteration count out of range");
    if (n == 0)
        usage(argv0, "iteration count must be at least 1");
    return n;
}

int main(int argc, char **argv) {
    /* Failures go to stderr and progress to stdout; unbuffering keeps the two in
     * true order when the run is piped to a log or through Valgrind, and keeps
     * the last line before an abort from being lost. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The iteration count is the only argument, and the Makefile is the only
     * caller that passes one; there is deliberately no seed override. */
    if (argc > 2)
        usage(argv[0], "too many arguments");
    if (argc == 2)
        g_iters = parse_iters(argv[0], argv[1]);

    g_rng_state = DEFAULT_SEED;

    /* The tree owns its values: releasing them on overwrite, delete, and
     * destroy is part of the contract under test, so any double free or
     * use-after-free on that path shows up under ASan and Valgrind. */
    rbtree_t *t = rb_create(free);
    if (!t)
        fail(__LINE__, "rb_create returned NULL");

    deep_check(t);

    /* invariant: after every iteration the tree and the model agree on the
     * value and payload of every key either of them holds */
    for (g_iter = 1; g_iter <= g_iters; ++g_iter) {
        char key[KEY_BUF];
        make_key(key, sizeof key, rng_next() % KEY_UNIVERSE);
        g_key = key;

        /* 40 insert / 30 find / 30 delete: enough delete pressure that the tree
         * keeps churning through rebalancing instead of saturating the key
         * universe and going quiet. */
        uint32_t roll = rng_next() % 100;
        if (roll < 40) {
            g_op = "insert";
            do_insert(t, key);
        } else if (roll < 70) {
            g_op = "find";
            do_find(t, key);
        } else {
            g_op = "delete";
            do_delete(t, key);
        }

        if (g_iter % VALIDATE_EVERY == 0)
            deep_check(t);

        g_key = "<none>"; /* key is about to leave scope */
    }

    g_op = "<teardown>";
    deep_check(t);

    rb_destroy(t); /* releases every remaining value through value_free */
    ref_clear();

    printf("fuzz: %lu iterations, seed %u, ok\n", g_iters, DEFAULT_SEED);
    printf("      insert=%lu overwrite=%lu find(hit=%lu miss=%lu) delete(hit=%lu miss=%lu)\n",
           g_n_insert, g_n_overwrite, g_n_find_hit, g_n_find_miss, g_n_delete_hit,
           g_n_delete_miss);
    return 0;
}
