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

/* Rotations. Both are purely structural: they move no keys, allocate nothing,
 * and so cannot fail. Leaves are NULL rather than a shared sentinel, so the
 * child that changes parents is re-parented only when it exists. */

/* Precondition: x->right != NULL. */
static void rotate_left(rbtree_t *t, struct rb_node *x) {
    struct rb_node *y = x->right;

    x->right = y->left;
    if (y->left)
        y->left->parent = x;

    y->parent = x->parent;
    if (!x->parent)
        t->root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;

    y->left   = x;
    x->parent = y;
}

/* Precondition: x->left != NULL. Mirror of rotate_left. Written out rather
 * than parameterized by direction: struct rb_node names its children instead
 * of holding an array, so a shared body would mean reshaping the node. */
static void rotate_right(rbtree_t *t, struct rb_node *x) {
    struct rb_node *y = x->left;

    x->left = y->right;
    if (y->right)
        y->right->parent = x;

    y->parent = x->parent;
    if (!x->parent)
        t->root = y;
    else if (x == x->parent->right)
        x->parent->right = y;
    else
        x->parent->left = y;

    y->right  = x;
    x->parent = y;
}

/* Restores the red-black invariants after n has been linked in as a red leaf.
 * Allocates nothing, so rb_insert's failure paths are unaffected by it. */
static void rb_insert_fixup(rbtree_t *t, struct rb_node *n) {
    /* invariant: the tree satisfies every red-black property except that n is
     * red and may have a red parent; n climbs toward the root each iteration */
    while (n->parent && n->parent->color == RB_RED) {
        struct rb_node *p = n->parent;
        /* The root is black on entry and made black again on exit, so a red
         * parent is never the root and its own parent always exists. */
        struct rb_node *g = p->parent;

        if (p == g->left) {
            struct rb_node *u = g->right; /* NULL uncle counts as black */
            if (u && u->color == RB_RED) {
                /* Push the grandparent's blackness down to both children: the
                 * black-height is unchanged and the violation moves up two. */
                p->color = RB_BLACK;
                u->color = RB_BLACK;
                g->color = RB_RED;
                n        = g;
            } else {
                if (n == p->right) {
                    /* Inner child: rotate it out to the straight-line case. */
                    n = p;
                    rotate_left(t, n);
                    p = n->parent;
                }
                p->color = RB_BLACK;
                g->color = RB_RED;
                rotate_right(t, g);
            }
        } else {
            struct rb_node *u = g->left;
            if (u && u->color == RB_RED) {
                p->color = RB_BLACK;
                u->color = RB_BLACK;
                g->color = RB_RED;
                n        = g;
            } else {
                if (n == p->left) {
                    n = p;
                    rotate_right(t, n);
                    p = n->parent;
                }
                p->color = RB_BLACK;
                g->color = RB_RED;
                rotate_left(t, g);
            }
        }
    }

    /* The recoloring case can leave the root red, and a rotation can install a
     * new root; blackening it unconditionally settles both and never breaks
     * the black-height, since it lies on every path equally. */
    t->root->color = RB_BLACK;
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

/* BST descent, then rb_insert_fixup to restore the red-black invariants. The
 * descent runs before any allocation: that leaves the overwrite path
 * allocation-free, hence unable to fail partway. */
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
    n->color  = RB_RED; /* red keeps every path's black-height unchanged */

    if (!parent)
        t->root = n;
    else if (cmp < 0)
        parent->left = n;
    else
        parent->right = n;
    ++t->size;

    /* Runs only once the node is fully linked and counted, and allocates
     * nothing itself, so no failure can be raised past this point. */
    rb_insert_fixup(t, n);
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

/* Returns the subtree's black-height counting NULL leaves as black, or -1 if
 * any invariant fails. lo and hi are exclusive key bounds, NULL meaning
 * unbounded; threading them down catches an ordering break between nodes that
 * are not parent and child, which a parent-only comparison would miss. They
 * point into nodes higher up the same walk, so they outlive the recursion. */
static int node_validate(const struct rb_node *n, const struct rb_node *parent,
                         const char *lo, const char *hi, size_t *count) {
    if (!n)
        return 1;

    /* A rotation that relinks a child but forgets its back-pointer leaves the
     * tree perfectly ordered and still fails here. */
    if (n->parent != parent)
        return -1;
    if (lo && strcmp(lo, n->key) >= 0)
        return -1;
    if (hi && strcmp(n->key, hi) >= 0)
        return -1;
    if (n->color == RB_RED) {
        if ((n->left && n->left->color == RB_RED) ||
            (n->right && n->right->color == RB_RED))
            return -1;
    }

    ++*count;

    int left  = node_validate(n->left, n, lo, n->key, count);
    int right = node_validate(n->right, n, n->key, hi, count);
    if (left < 0 || right < 0 || left != right)
        return -1;

    return left + (n->color == RB_BLACK ? 1 : 0);
}

int rb_validate(const rbtree_t *t) {
    if (t->root && (t->root->color != RB_BLACK || t->root->parent != NULL))
        return -1;

    size_t count = 0;
    if (node_validate(t->root, NULL, NULL, NULL, &count) < 0)
        return -1;

    /* Beyond the red-black properties proper, but a size that has drifted from
     * the nodes actually reachable is a defect and this is the cheapest place
     * to notice it. */
    return count == t->size ? 0 : -1;
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
