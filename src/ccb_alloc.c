// ccb_alloc implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"

#define CCB_ARENA_BLOCK 65536

typedef struct ccb_block {
  struct ccb_block *next;
  size_t used;
  size_t cap;
  unsigned char data[];
} ccb_block;

struct ccb_arena {
  ccb_block *head;
};

static void ccb_oom(const char *what, size_t n, const char *file, int line) {
  fprintf(stderr, "ccb: out of memory: %s (%zu bytes) at %s:%d\n", what, n, file, line);
  abort();
}

void *ccb_xmalloc(size_t n) {
  void *p = malloc(n == 0 ? 1 : n);
  if (p == NULL) {
    ccb_oom("malloc", n, __FILE__, __LINE__);
  }
  return p;
}

void *ccb_xcalloc(size_t n, size_t sz) {
  void *p = calloc(n == 0 ? 1 : n, sz == 0 ? 1 : sz);
  if (p == NULL) {
    size_t total = 0;
    if (__builtin_mul_overflow(n, sz, &total)) {
      total = (size_t)-1;
    }
    ccb_oom("calloc", total, __FILE__, __LINE__);
  }
  return p;
}

void *ccb_xrealloc(void *p, size_t n) {
  void *q = realloc(p, n == 0 ? 1 : n);
  if (q == NULL) {
    ccb_oom("realloc", n, __FILE__, __LINE__);
  }
  return q;
}

char *ccb_xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = ccb_xmalloc(n);
  memcpy(p, s, n);
  return p;
}

char *ccb_xstrndup(const char *s, size_t n) {
  char *p = ccb_xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

ccb_arena *ccb_arena_create(void) {
  ccb_arena *a = ccb_xmalloc(sizeof(*a));
  a->head = NULL;
  return a;
}

void ccb_arena_destroy(ccb_arena *a) {
  ccb_block *b = a->head;
  while (b != NULL) {
    ccb_block *next = b->next;
    free(b);
    b = next;
  }
  free(a);
}

static void *ccb_arena_alloc_from(ccb_arena *a, size_t n, size_t align) {
  // Round up to the requested alignment (power of two).
  size_t aligned = (n + align - 1) & ~(align - 1);
  ccb_block *b = a->head;
  if (b != NULL && b->cap - b->used >= aligned) {
    void *p = b->data + b->used;
    b->used += aligned;
    return p;
  }
  size_t cap = CCB_ARENA_BLOCK;
  if (aligned > cap) {
    cap = aligned;
  }
  ccb_block *nb = ccb_xmalloc(sizeof(*nb) + cap);
  nb->next = a->head;
  nb->used = aligned;
  nb->cap = cap;
  a->head = nb;
  return nb->data;
}

void *ccb_arena_alloc(ccb_arena *a, size_t n) {
  return ccb_arena_alloc_from(a, n, 16);
}

char *ccb_arena_strdup(ccb_arena *a, const char *s) {
  size_t n = strlen(s) + 1;
  char *p = ccb_arena_alloc(a, n);
  memcpy(p, s, n);
  return p;
}

char *ccb_arena_strndup(ccb_arena *a, const char *s, size_t n) {
  char *p = ccb_arena_alloc(a, n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}
