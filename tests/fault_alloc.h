#ifndef FAULT_ALLOC_H
#define FAULT_ALLOC_H
#include <stddef.h>

/* Single interception point for every heap allocation in src/.
 *
 * Contract (holds now and once fault injection is added behind it):
 *   rb_malloc  returns NULL on failure; callers must handle it and unwind
 *              completely. A zero size behaves as malloc(0) does.
 *   rb_free    accepts NULL as a no-op.
 *
 * The current bodies are pure pass-throughs to malloc/free. */
void *rb_malloc(size_t size);
void  rb_free(void *ptr);

#endif
