/* Source-relative path: the build links every .c in one gcc invocation and
 * resolves -Iinclude against make's CWD, so do not depend on the search path. */
#include "../include/rbtree.h"

#include "../tests/fault_alloc.h"

#include <string.h>

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

/* strcmp for every comparison: its unsigned-char ordering is total over all
 * byte values, and mixing it with a signed comparison elsewhere would split
 * the tree's order. Comparing to the NUL terminator also makes a key an
 * ordinary neighbour of its own prefixes rather than a special case. */
static struct rb_node *node_find(struct rb_node *n, const char *key) {
    /* invariant: key, if present at all, lies in the subtree rooted at n */
    while (n) {
        int cmp = strcmp(key, n->key);
        if (cmp == 0)
            return n;
        n = (cmp < 0) ? n->left : n->right;
    }
    return NULL;
}

/* Remaining M0 stubs. Each one returns the failure sentinel its header
 * contract defines, so an unimplemented path can never be mistaken for a
 * working one; they are replaced a function at a time. */

rbtree_t *rb_create(rb_value_free_fn value_free) {
    rbtree_t *t = rb_malloc(sizeof *t);
    if (!t)
        return NULL;
    /* rb_malloc does not zero, so every field is set explicitly. */
    t->root       = NULL;
    t->size       = 0;
    t->value_free = value_free;
    return t;
}

rbtree_t *rb_create_pooled(rb_value_free_fn value_free) {
    (void)value_free;
    return NULL; /* M0 stub */
}

/* M0: plain BST descent, no red-black fixup, so ordered input builds a
 * degenerate spine. The descent runs before any allocation: that leaves the
 * overwrite path allocation-free, hence unable to fail partway. */
int rb_insert(rbtree_t *t, const char *key, void *value) {
    struct rb_node *parent = NULL;
    int             cmp    = 0;

    /* invariant: parent is the last node compared, and cmp its comparison,
     * so on exit key belongs exactly where the walk fell off the tree */
    for (struct rb_node *n = t->root; n;) {
        parent = n;
        cmp    = strcmp(key, n->key);
        if (cmp == 0) {
            /* Re-inserting the stored pointer under its own key must not free
             * what it is about to store. */
            if (t->value_free && value != n->value)
                t->value_free(n->value);
            n->value = value;
            return 0;
        }
        n = (cmp < 0) ? n->left : n->right;
    }

    struct rb_node *n = rb_malloc(sizeof *n);
    if (!n)
        goto fail;

    size_t len = strlen(key) + 1;
    n->key     = rb_malloc(len);
    if (!n->key)
        goto fail_node;
    memcpy(n->key, key, len);

    n->parent = parent;
    n->left   = NULL;
    n->right  = NULL;
    n->value  = value;
    n->color  = RB_RED; /* fresh nodes are red so M1's fixup drops in unchanged */

    if (!parent)
        t->root = n;
    else if (cmp < 0)
        parent->left = n;
    else
        parent->right = n;
    ++t->size;
    return 0;

fail_node:
    rb_free(n);
fail:
    /* Nothing was linked and size never moved: the tree is exactly as it was
     * and value is still the caller's to release. */
    return -1;
}

void *rb_find(const rbtree_t *t, const char *key) {
    /* const on the member qualifies the pointer, not the nodes it reaches, so
     * the shared descent needs no cast here. */
    struct rb_node *n = node_find(t->root, key);
    return n ? n->value : NULL;
}

int rb_delete(rbtree_t *t, const char *key) {
    (void)t;
    (void)key;
    return -1; /* M0 stub: absent */
}

size_t rb_size(const rbtree_t *t) {
    return t->size;
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

/* Walks down to a leaf, frees it, and climbs back through parent, so teardown
 * needs no stack and no auxiliary storage: this is already the O(1)-space,
 * non-recursive form Mutation 3 requires. */
void rb_destroy(rbtree_t *t) {
    if (!t)
        return;

    /* invariant: n is the deepest node not yet freed on the current path, and
     * every node already freed has been unlinked from its parent, so no freed
     * node is ever reachable again */
    struct rb_node *n = t->root;
    while (n) {
        if (n->left) {
            n = n->left;
            continue;
        }
        if (n->right) {
            n = n->right;
            continue;
        }
        struct rb_node *p = n->parent;
        if (p) {
            if (p->left == n)
                p->left = NULL;
            else
                p->right = NULL;
        }
        if (t->value_free)
            t->value_free(n->value);
        rb_free(n->key);
        rb_free(n);
        n = p;
    }
    rb_free(t);
}

rbtree_t *rb_snapshot(rbtree_t *t) {
    (void)t;
    return NULL; /* M0 stub */
}
