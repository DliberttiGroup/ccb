// Tests for ccb_cache.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_cache.h"

#define B(rel) ccb_test_path(dir, rel)

int main(void) {
  char dir[256];
  ccb_test_tmpdir(dir, sizeof dir, "cache");
  ccb_remove_recursive(dir);
  CHECK(ccb_mkdir_p(dir) == CCB_OK);
  CHECK(ccb_write_file(B("f.txt"), "data", 4) == CCB_OK);

  // Missing cache loads as empty.
  {
    ccb_cache *c = NULL;
    CHECK(ccb_cache_load(B("nope.json"), &c) == CCB_OK);
    CHECK(c != NULL);
    CHECK(c->tool_fingerprint == NULL);
    CHECK(ccb_cache_step_fp(c, "kotlinc") == NULL);
    ccb_cache_free(c);
  }
  // Round-trip.
  {
    ccb_cache *c = NULL;
    CHECK(ccb_cache_load(B("nope.json"), &c) == CCB_OK);
    c->tool_fingerprint = ccb_xstrdup("bt=35.0.0|kt=2.4.21");
    ccb_cache_set_step(c, "kotlinc", "aaa");
    ccb_cache_set_step(c, "d8", "bbb");
    ccb_cache_set_file(c, "src/Main.kt", "deadbeef");
    CHECK(ccb_cache_save(c, B("cache.json")) == CCB_OK);
    ccb_cache_free(c);
    CHECK(ccb_cache_load(B("cache.json"), &c) == CCB_OK);
    CHECK_STR_EQ(c->tool_fingerprint, "bt=35.0.0|kt=2.4.21");
    CHECK_STR_EQ(ccb_cache_step_fp(c, "kotlinc"), "aaa");
    CHECK_STR_EQ(ccb_cache_step_fp(c, "d8"), "bbb");
    CHECK_STR_EQ(ccb_cache_file_hash(c, "src/Main.kt"), "deadbeef");
    // Overwrite keeps one entry per key.
    ccb_cache_set_step(c, "kotlinc", "ccc");
    CHECK_STR_EQ(ccb_cache_step_fp(c, "kotlinc"), "ccc");
    CHECK(c->step_count == 2);
    ccb_cache_free(c);
  }
  // Corrupt cache loads as empty, not an error.
  {
    CHECK(ccb_write_file(B("bad.json"), "{not json", 9) == CCB_OK);
    ccb_cache *c = NULL;
    CHECK(ccb_cache_load(B("bad.json"), &c) == CCB_OK);
    CHECK(c->tool_fingerprint == NULL);
    ccb_cache_free(c);
  }
  // File hashing + fingerprint determinism.
  {
    char *h = ccb_cache_hash_file(B("f.txt"));
    CHECK(h != NULL);
    char expect[65];
    ccb_sha256_hex("data", 4, expect);
    CHECK_STR_EQ(h, expect);
    free(h);
    CHECK(ccb_cache_hash_file(B("missing.txt")) == NULL);
    const char *keys[] = { "a", "b" };
    const char *vals[] = { "1", "2" };
    char *f1 = ccb_cache_fingerprint(keys, vals, 2);
    char *f2 = ccb_cache_fingerprint(keys, vals, 2);
    CHECK_STR_EQ(f1, f2);
    const char *vals2[] = { "1", "3" };
    char *f3 = ccb_cache_fingerprint(keys, vals2, 2);
    CHECK(strcmp(f1, f3) != 0);
    free(f1);
    free(f2);
    free(f3);
  }
  ccb_remove_recursive(dir);
  TEST_MAIN_END();
}
