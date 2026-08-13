#include "fault_alloc.h"

#include <stdlib.h>

void *rb_malloc(size_t size) {
    return malloc(size);
}

void rb_free(void *ptr) {
    free(ptr);
}
