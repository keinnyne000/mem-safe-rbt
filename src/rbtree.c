/* Source-relative path: the build links every .c in one gcc invocation and
 * resolves -Iinclude against make's CWD, so do not depend on the search path. */
#include "../include/rbtree.h"

/* Red-black node. Leaves are NULL, not a shared sentinel: a writable
 * shared sentinel would hide dangling leaf pointers from ASan/Valgrind
 * and cannot be shared across snapshots. */
enum rb_color : unsigned char {
    RB_RED   = 0, /* zeroed storage is already the color of a fresh node */
    RB_BLACK = 1,
};

struct rb_node {
    struct rb_node *parent; /* NULL iff this node is t->root */
    struct rb_node *left;   /* NULL == black leaf */
    struct rb_node *right;  /* NULL == black leaf */
    char           *key;    /* owned NUL-terminated copy, never NULL */
    void           *value;  /* opaque; freed via t->value_free */
    enum rb_color   color;
};

/* The M2 slab pool hands out fixed-stride nodes; pin the layout it assumes. */
_Static_assert(sizeof(struct rb_node) == 48, "node stride");

struct rbtree {
    struct rb_node   *root;       /* NULL when empty */
    size_t            size;       /* live node count; rb_size() returns this */
    rb_value_free_fn  value_free; /* NULL => values not owned */
};

/* M0 stubs for the public API. Each one returns the failure sentinel its
 * header contract defines, so an unimplemented path can never be mistaken
 * for a working one; they are replaced a function at a time. */

rbtree_t *rb_create(rb_value_free_fn value_free) {
    (void)value_free;
    return NULL; /* M0 stub */
}

rbtree_t *rb_create_pooled(rb_value_free_fn value_free) {
    (void)value_free;
    return NULL; /* M0 stub */
}

int rb_insert(rbtree_t *t, const char *key, void *value) {
    (void)t;
    (void)key;
    (void)value;
    return -1; /* M0 stub: tree unchanged, value not consumed */
}

void *rb_find(const rbtree_t *t, const char *key) {
    (void)t;
    (void)key;
    return NULL; /* M0 stub: absent */
}

int rb_delete(rbtree_t *t, const char *key) {
    (void)t;
    (void)key;
    return -1; /* M0 stub: absent */
}

size_t rb_size(const rbtree_t *t) {
    (void)t;
    return 0; /* M0 stub */
}

void rb_foreach(const rbtree_t *t,
                void (*fn)(const char *key, void *value, void *ctx),
                void *ctx) {
    (void)t;
    (void)fn;
    (void)ctx;
    /* M0 stub: visits nothing */
}

int rb_validate(const rbtree_t *t) {
    (void)t;
    return -1; /* M0 stub: invariants not established */
}

void rb_destroy(rbtree_t *t) {
    (void)t;
    /* M0 stub: nothing allocated yet, so nothing to free */
}

rbtree_t *rb_snapshot(rbtree_t *t) {
    (void)t;
    return NULL; /* M0 stub */
}
