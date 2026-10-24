// Tests for ccb_str, ccb_slice, ccb_alloc.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_slice.h"

int main(void) {
  // str
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "hello");
    ccb_str_append_c(&s, ' ');
    ccb_str_appendf(&s, "%s %d", "world", 42);
    CHECK_STR_EQ(s.buf, "hello world 42");
    CHECK(s.len == 14);
    char *owned = ccb_str_steal(&s);
    CHECK_STR_EQ(owned, "hello world 42");
    free(owned);
    ccb_str_free(&s);
  }
  // slice
  {
    ccb_slice a = ccb_slice_from("  hello  ");
    ccb_slice t = ccb_slice_trim(a);
    CHECK(ccb_slice_eq(t, "hello"));
    CHECK(ccb_slice_starts_with(a, "  h"));
    long v = 0;
    CHECK(ccb_slice_to_long(ccb_slice_from("123"), &v) == 0 && v == 123);
    CHECK(ccb_slice_to_long(ccb_slice_from("12x"), &v) != 0);
    CHECK(ccb_slice_to_long(ccb_slice_from(""), &v) != 0);
    char *s = ccb_slice_to_str(t);
    CHECK_STR_EQ(s, "hello");
    free(s);
  }
  // arena
  {
    ccb_arena *ar = ccb_arena_create();
    char *a = ccb_arena_strdup(ar, "one");
    char *b = ccb_arena_strndup(ar, "two-three", 3);
    CHECK_STR_EQ(a, "one");
    CHECK_STR_EQ(b, "two");
    // Many small allocations force new blocks; pointers must stay valid.
    char *first = ccb_arena_strdup(ar, "first");
    for (int i = 0; i < 5000; i++) {
      ccb_arena_alloc(ar, 32);
    }
    CHECK_STR_EQ(first, "first");
    ccb_arena_destroy(ar);
  }
  // xstrdup / xstrndup
  {
    char *d = ccb_xstrdup("abc");
    CHECK_STR_EQ(d, "abc");
    free(d);
    char *e = ccb_xstrndup("abcdef", 3);
    CHECK_STR_EQ(e, "abc");
    free(e);
  }
  TEST_MAIN_END();
}
