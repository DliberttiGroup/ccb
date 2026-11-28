// Tests for the Step 10 reproducibility contract (docs/reproducibility.md):
// ZIP writer byte determinism + fixed timestamps + insertion order,
// fingerprint order behavior, cache-manifest canonical save order,
// lockfile sorted output. No real Android build: fast.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cache.h"
#include "ccb_config.h"
#include "ccb_diag.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_zip.h"

#define B(rel) ccb_test_path(dir, rel)

static int files_equal(const char *p1, const char *p2) {
  char *t1 = NULL;
  char *t2 = NULL;
  size_t l1 = 0;
  size_t l2 = 0;
  if (ccb_read_file(p1, &t1, &l1) != CCB_OK) {
    return 0;
  }
  if (ccb_read_file(p2, &t2, &l2) != CCB_OK) {
    free(t1);
    return 0;
  }
  int eq = l1 == l2 && memcmp(t1, t2, l1) == 0;
  free(t1);
  free(t2);
  return eq;
}

int main(void) {
  char dir[256];
  ccb_test_tmpdir(dir, sizeof dir, "repro");

  // ZIP writer: two writes of the same entries are byte-identical,
  // entry order is insertion order, timestamps are the fixed 1980
  // DOS date (time 0x0000, date 0x0021) in the local header.
  {
    for (int run = 0; run < 2; run++) {
      char *path = B(run == 0 ? "a.zip" : "b.zip");
      ccb_zip_writer w;
      CHECK(ccb_zip_writer_init(&w, path) == CCB_OK);
      CHECK(ccb_zip_writer_add_data(&w, "b.txt", "bee", 3) == CCB_OK);
      CHECK(ccb_zip_writer_add_data(&w, "a.txt", "ay", 2) == CCB_OK);
      CHECK(ccb_zip_writer_add_data(&w, "c/d.txt", "", 0) == CCB_OK);
      CHECK(ccb_zip_writer_finish(&w) == CCB_OK);
    }
    CHECK(files_equal(B("a.zip"), B("b.zip")));
    char *raw = NULL;
    size_t raw_len = 0;
    CHECK(ccb_read_file(B("a.zip"), &raw, &raw_len) == CCB_OK);
    CHECK(raw_len > 14);
    CHECK(raw[0] == 0x50 && raw[1] == 0x4b && raw[2] == 0x03 && raw[3] == 0x04);
    CHECK(raw[10] == 0 && raw[11] == 0);
    CHECK((unsigned char)raw[12] == 0x21 && raw[13] == 0);
    free(raw);
    ccb_zip_archive *a = NULL;
    CHECK(ccb_zip_open(B("a.zip"), &a) == CCB_OK);
    CHECK(a->count == 3);
    CHECK_STR_EQ(a->entries[0].name, "b.txt");
    CHECK_STR_EQ(a->entries[1].name, "a.txt");
    CHECK_STR_EQ(a->entries[2].name, "c/d.txt");
    ccb_zip_close(a);
  }

  // Fingerprint: same pairs in the same order hash equal; the hash
  // is order-sensitive by construction (key=value lines in insertion
  // order), so a reordered builder hashes differently. Builders add
  // keys in fixed code order, which is what makes equal inputs hash
  // equal; the reordered case pins the order-sensitivity instead of
  // hiding it.
  {
    const char *keys[] = { "a", "b" };
    const char *vals[] = { "1", "2" };
    char *f1 = ccb_cache_fingerprint(keys, vals, 2);
    char *f2 = ccb_cache_fingerprint(keys, vals, 2);
    CHECK_STR_EQ(f1, f2);
    const char *keys_r[] = { "b", "a" };
    const char *vals_r[] = { "2", "1" };
    char *f3 = ccb_cache_fingerprint(keys_r, vals_r, 2);
    CHECK(strcmp(f1, f3) != 0);
    free(f1);
    free(f2);
    free(f3);
    char *f0 = ccb_cache_fingerprint(NULL, NULL, 0);
    CHECK_STR_EQ(f0,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    free(f0);
  }

  // Cache manifest: save order is canonical (sorted by key), so two
  // caches holding the same entries recorded in opposite orders save
  // byte-identical files, and a load + save round-trip is stable.
  {
    ccb_cache *c1 = NULL;
    CHECK(ccb_cache_load(B("nope.json"), &c1) == CCB_OK);
    ccb_cache_set_step(c1, "zeta", "z1");
    ccb_cache_set_step(c1, "alpha", "a1");
    ccb_cache_set_file(c1, "z.kt", "zh");
    ccb_cache_set_file(c1, "a.kt", "ah");
    CHECK(ccb_cache_save(c1, B("c1.json")) == CCB_OK);
    ccb_cache_free(c1);
    ccb_cache *c2 = NULL;
    CHECK(ccb_cache_load(B("nope.json"), &c2) == CCB_OK);
    ccb_cache_set_step(c2, "alpha", "a1");
    ccb_cache_set_step(c2, "zeta", "z1");
    ccb_cache_set_file(c2, "a.kt", "ah");
    ccb_cache_set_file(c2, "z.kt", "zh");
    CHECK(ccb_cache_save(c2, B("c2.json")) == CCB_OK);
    ccb_cache_free(c2);
    CHECK(files_equal(B("c1.json"), B("c2.json")));
    ccb_cache *c3 = NULL;
    CHECK(ccb_cache_load(B("c1.json"), &c3) == CCB_OK);
    CHECK_STR_EQ(ccb_cache_step_fp(c3, "alpha"), "a1");
    CHECK_STR_EQ(ccb_cache_step_fp(c3, "zeta"), "z1");
    CHECK_STR_EQ(ccb_cache_file_hash(c3, "a.kt"), "ah");
    CHECK(ccb_cache_save(c3, B("c3.json")) == CCB_OK);
    ccb_cache_free(c3);
    CHECK(files_equal(B("c1.json"), B("c3.json")));
  }

  // Lockfile: sorted by group:artifact, so the file depends only on
  // the resolved set, never on declaration order. Two writes from
  // the same entries in different orders are byte-identical.
  {
    ccb_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_root = ccb_xstrdup(dir);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_maven_entry *entries = ccb_xmalloc(3 * sizeof(*entries));
    memset(entries, 0, 3 * sizeof(*entries));
    entries[0].group = ccb_xstrdup("com.zeta");
    entries[0].artifact = ccb_xstrdup("zlib");
    entries[0].version = ccb_xstrdup("2.0");
    entries[0].packaging = ccb_xstrdup("aar");
    entries[0].sha256 = ccb_xstrdup("aa11");
    entries[0].path = ccb_xstrdup("/cache/zlib.aar");
    entries[1].group = ccb_xstrdup("com.alpha");
    entries[1].artifact = ccb_xstrdup("alib");
    entries[1].version = ccb_xstrdup("1.0");
    entries[1].packaging = ccb_xstrdup("jar");
    entries[1].sha256 = ccb_xstrdup("bb22");
    entries[1].path = ccb_xstrdup("/cache/alib.jar");
    entries[2].group = ccb_xstrdup("com.alpha");
    entries[2].artifact = ccb_xstrdup("blib");
    entries[2].version = ccb_xstrdup("1.5");
    entries[2].packaging = ccb_xstrdup("aar");
    entries[2].sha256 = ccb_xstrdup("cc33");
    entries[2].path = ccb_xstrdup("/cache/blib.aar");
    ccb_maven_result r;
    memset(&r, 0, sizeof(r));
    r.entries = entries;
    r.entries_count = 3;
    CHECK(ccb_maven_write_lockfile(&cfg, &r, &diags) == CCB_OK);
    char *text = NULL;
    size_t tlen = 0;
    CHECK(ccb_read_file(B("ccb.lock"), &text, &tlen) == CCB_OK);
    CHECK_STR_EQ(text,
      "# ccb.lock: resolved Maven tree, generated by `ccb setup`.\n"
      "# Commit this file. `ccb check` and `ccb build` enforce it: any\n"
      "# deviation from these versions or hashes fails with CCB-E3023.\n"
      "# ccb-lock-version: 1\n"
      "com.alpha:alib:1.0 jar bb22\n"
      "com.alpha:blib:1.5 aar cc33\n"
      "com.zeta:zlib:2.0 aar aa11\n");
    free(text);
    // Keep a copy of the first write: the second write, from the
    // same entries in sorted declaration order, must match it.
    CHECK(ccb_read_file(B("ccb.lock"), &text, &tlen) == CCB_OK);
    CHECK(ccb_write_file(B("lock1.txt"), text, tlen) == CCB_OK);
    free(text);
    // Same entries in sorted declaration order: identical file.
    ccb_maven_entry *entries2 = ccb_xmalloc(3 * sizeof(*entries2));
    memset(entries2, 0, 3 * sizeof(*entries2));
    entries2[0].group = ccb_xstrdup("com.alpha");
    entries2[0].artifact = ccb_xstrdup("alib");
    entries2[0].version = ccb_xstrdup("1.0");
    entries2[0].packaging = ccb_xstrdup("jar");
    entries2[0].sha256 = ccb_xstrdup("bb22");
    entries2[0].path = ccb_xstrdup("/cache/alib.jar");
    entries2[1].group = ccb_xstrdup("com.alpha");
    entries2[1].artifact = ccb_xstrdup("blib");
    entries2[1].version = ccb_xstrdup("1.5");
    entries2[1].packaging = ccb_xstrdup("aar");
    entries2[1].sha256 = ccb_xstrdup("cc33");
    entries2[1].path = ccb_xstrdup("/cache/blib.aar");
    entries2[2].group = ccb_xstrdup("com.zeta");
    entries2[2].artifact = ccb_xstrdup("zlib");
    entries2[2].version = ccb_xstrdup("2.0");
    entries2[2].packaging = ccb_xstrdup("aar");
    entries2[2].sha256 = ccb_xstrdup("aa11");
    entries2[2].path = ccb_xstrdup("/cache/zlib.aar");
    ccb_maven_result r2;
    memset(&r2, 0, sizeof(r2));
    r2.entries = entries2;
    r2.entries_count = 3;
    CHECK(ccb_maven_write_lockfile(&cfg, &r2, &diags) == CCB_OK);
    CHECK(files_equal(B("lock1.txt"), B("ccb.lock")));
    ccb_maven_entries_free(entries2, 3);
    ccb_maven_entries_free(entries, 3);
    ccb_diag_list_free(&diags);
    free(cfg.project_root);
  }

  ccb_remove_recursive(dir);
  TEST_MAIN_END();
}
