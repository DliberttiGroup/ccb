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
  // Atomic publication + corrupt-cache fallback (Step 2).
  {
    // ccb_write_file_atomic: write, overwrite, read back; no temp
    // file is left behind after either write.
    CHECK(ccb_write_file_atomic(B("atomic.txt"), "hello", 5) == CCB_OK);
    char *t = NULL;
    size_t tl = 0;
    CHECK(ccb_read_file(B("atomic.txt"), &t, &tl) == CCB_OK);
    CHECK(tl == 5 && memcmp(t, "hello", 5) == 0);
    free(t);
    CHECK(ccb_write_file_atomic(B("atomic.txt"), "world!", 6) == CCB_OK);
    CHECK(ccb_read_file(B("atomic.txt"), &t, &tl) == CCB_OK);
    CHECK(tl == 6 && memcmp(t, "world!", 6) == 0);
    free(t);
    // A cache saved atomically over an existing cache replaces it.
    ccb_cache *c = NULL;
    CHECK(ccb_cache_load(B("nope.json"), &c) == CCB_OK);
    ccb_cache_set_step(c, "dex", "old");
    CHECK(ccb_cache_save(c, B("cache2.json")) == CCB_OK);
    ccb_cache_set_step(c, "dex", "new");
    CHECK(ccb_cache_save(c, B("cache2.json")) == CCB_OK);
    ccb_cache_free(c);
    CHECK(ccb_cache_load(B("cache2.json"), &c) == CCB_OK);
    CHECK_STR_EQ(ccb_cache_step_fp(c, "dex"), "new");
    ccb_cache_free(c);
    // No temp file left behind by any of the saves above.
    DIR *d = opendir(dir);
    CHECK(d != NULL);
    struct dirent *e = NULL;
    int tmp_left = 0;
    while ((e = readdir(d)) != NULL) {
      if (strstr(e->d_name, ".tmp-") != NULL) {
        tmp_left = 1;
      }
    }
    closedir(d);
    CHECK(tmp_left == 0);
  }
  // Corrupt / truncated / wrong-version caches load as empty.
  {
    const char *bad[] = {
      // Truncated mid-object: what an interrupted non-atomic save
      // used to leave behind.
      "{\n  \"version\": 1,\n  \"steps\": {\"dex\": \"abc",
      // Wrong version, version as a string, non-object roots.
      "{\"version\": 2, \"steps\": {}}",
      "{\"version\": \"1\", \"steps\": {}}",
      "[]",
      "\"x\"",
      "",
    };
    for (size_t i = 0; i < CCB_ARRAY_LEN(bad); i++) {
      CHECK(ccb_write_file(B("bad2.json"), bad[i], strlen(bad[i])) == CCB_OK);
      ccb_cache *c = NULL;
      CHECK(ccb_cache_load(B("bad2.json"), &c) == CCB_OK);
      CHECK(c->tool_fingerprint == NULL);
      CHECK(ccb_cache_step_fp(c, "dex") == NULL);
      ccb_cache_free(c);
    }
    // Non-string entries are ignored, string entries still load.
    CHECK(ccb_write_file(B("mixed.json"),
      "{\"version\": 1, \"tools\": 42, "
      "\"steps\": {\"dex\": 42, \"code\": \"ok\"}}", 63) == CCB_OK);
    ccb_cache *c = NULL;
    CHECK(ccb_cache_load(B("mixed.json"), &c) == CCB_OK);
    CHECK(c->tool_fingerprint == NULL);
    CHECK(ccb_cache_step_fp(c, "dex") == NULL);
    CHECK_STR_EQ(ccb_cache_step_fp(c, "code"), "ok");
    ccb_cache_free(c);
  }
  ccb_remove_recursive(dir);
  TEST_MAIN_END();
}
