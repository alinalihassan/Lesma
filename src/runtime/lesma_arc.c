/* Lesma class heap: refcount at object start (matches LLVM layout: first field i64). */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

void* lesma_arc_alloc(uint64_t total_size) {
  if (total_size < sizeof(int64_t)) {
    return NULL;
  }
  void* p = malloc((size_t)total_size);
  if (p == NULL) {
    return NULL;
  }
  atomic_store_explicit((_Atomic int64_t*)p, 1, memory_order_relaxed);
  return p;
}

void lesma_arc_retain(void* p) {
  if (p == NULL) {
    return;
  }
  (void)atomic_fetch_add_explicit((_Atomic int64_t*)p, 1, memory_order_relaxed);
}

typedef void (*lesma_arc_finalize_fn)(void*);

void lesma_arc_release(void* p, lesma_arc_finalize_fn finalize) {
  if (p == NULL) {
    return;
  }
  int64_t prev = atomic_fetch_sub_explicit((_Atomic int64_t*)p, 1, memory_order_acq_rel);
  if (prev == 1) {
    if (finalize != NULL) {
      finalize(p);
    } else {
      free(p);
    }
  }
}
