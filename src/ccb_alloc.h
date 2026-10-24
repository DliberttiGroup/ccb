// ccb_alloc: checked allocation, arena allocator, scope cleanup.
// Ownership rule: anything returned by ccb_x* is owned by the caller and
// must be freed with free(). Anything returned by ccb_arena_* is owned by
// the arena and must NOT be freed individually.
#ifndef CCB_ALLOC_H
#define CCB_ALLOC_H

#include "ccb_common.h"

// Abort with a file:line diagnostic on out-of-memory. Fail-fast beats a
// half-built APK: recovery mid-build is meaningless.
void *ccb_xmalloc(size_t n);
void *ccb_xcalloc(size_t n, size_t sz);
void *ccb_xrealloc(void *p, size_t n);
char *ccb_xstrdup(const char *s);
char *ccb_xstrndup(const char *s, size_t n);

typedef struct ccb_arena ccb_arena;

ccb_arena *ccb_arena_create(void);
void ccb_arena_destroy(ccb_arena *a);
void *ccb_arena_alloc(ccb_arena *a, size_t n);
char *ccb_arena_strdup(ccb_arena *a, const char *s);
char *ccb_arena_strndup(ccb_arena *a, const char *s, size_t n);

// Scope cleanup helper: CCB_AUTO_FREE char *p = ...; frees p on scope exit.
static inline void ccb_free_ptr(void *p) {
  free(*(void **)p);
}
#define CCB_AUTO_FREE __attribute__((cleanup(ccb_free_ptr)))

// Scope cleanup for arenas: CCB_AUTO_ARENA ccb_arena *a = ccb_arena_create();
static inline void ccb_arena_ptr_free(void *p) {
  ccb_arena *a = *(ccb_arena **)p;
  if (a != NULL) {
    ccb_arena_destroy(a);
  }
}
#define CCB_AUTO_ARENA __attribute__((cleanup(ccb_arena_ptr_free)))

#endif
