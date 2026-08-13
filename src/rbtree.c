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
