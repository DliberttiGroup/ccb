// Tests for the Step 7 boundaries: resolved dependencies
// (ccb_resolved), the single toolchain identity
// (ccb_sdk_toolchain_identity), and the shared fingerprint key
// groups. Fast: no toolchain, only temp files.

#include "test.h"
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_cache.h"
#include "ccb_resolved.h"
#include "ccb_pipeline_internal.h"

#define B(rel) ccb_test_path(base, rel)

static int builder_has_key(ccb_fp_builder *b, size_t i,
    const char *key) {
  return i < b->count && strcmp(b->keys[i], key) == 0;
}

int main(void) {
  char base[256];
  ccb_test_tmpdir(base, sizeof base, "resolved");
  ccb_remove_recursive(base);
  CHECK(ccb_mkdir_p(B("proj")) == CCB_OK);
  CHECK(ccb_write_file(B("proj/j.jar"), "jar-bytes", 9) == CCB_OK);
  CHECK(ccb_write_file(B("proj/k.jar"), "other-jar", 9) == CCB_OK);

  // --- ccb_resolved_from_config copies declared lists only ---
  {
    ccb_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.aars = ccb_xcalloc(2, sizeof(char *));
    cfg.aars[0] = ccb_xstrdup("x.aar");
    cfg.aars[1] = ccb_xstrdup("y.aar");
    cfg.aars_count = 2;
    cfg.jars = ccb_xcalloc(1, sizeof(char *));
    cfg.jars[0] = ccb_xstrdup("j.jar");
    cfg.jars_count = 1;
    ccb_resolved r;
    ccb_resolved_from_config(&r, &cfg);
    CHECK(r.aars_count == 2);
    CHECK(r.jars_count == 1);
    CHECK(r.plugin_jars_count == 0);
    CHECK_STR_EQ(r.aars[0], "x.aar");
    CHECK_STR_EQ(r.aars[1], "y.aar");
    CHECK_STR_EQ(r.jars[0], "j.jar");
    // Resolution appends land in resolved, never in the config...
    ccb_resolved_push_aar(&r, ccb_xstrdup("maven.aar"));
    ccb_resolved_push_jar(&r, ccb_xstrdup("maven.jar"));
    ccb_resolved_push_plugin_jar(&r, ccb_xstrdup("plug.jar"));
    CHECK(r.aars_count == 3);
    CHECK(r.jars_count == 2);
    CHECK(r.plugin_jars_count == 1);
    CHECK(cfg.aars_count == 2);
    CHECK(cfg.jars_count == 1);
    // ...and the copies are owned: freeing the config's lists
    // leaves the resolved lists intact. (ccb_config_free frees
    // the struct itself, so a stack config's lists are freed
    // by hand here.)
    ccb_free_str_array(cfg.aars, cfg.aars_count);
    ccb_free_str_array(cfg.jars, cfg.jars_count);
    CHECK_STR_EQ(r.aars[0], "x.aar");
    CHECK_STR_EQ(r.jars[0], "j.jar");
    ccb_resolved_free(&r);
    CHECK(r.aars_count == 0);
    CHECK(r.jars_count == 0);
    CHECK(r.plugin_jars_count == 0);
  }

  // --- single toolchain identity: one definition, exact string ---
  {
    ccb_config cfg;
    ccb_sdk sdk;
    memset(&cfg, 0, sizeof(cfg));
    memset(&sdk, 0, sizeof(sdk));
    cfg.compile_sdk = 35;
    sdk.build_tools_version = "35.0.0";
    sdk.aapt2 = "/sdk/build-tools/35.0.0/aapt2";
    sdk.zipalign = "/sdk/build-tools/35.0.0/zipalign";
    sdk.kotlin_version = "2.4.21";
    sdk.java_major = 17;
    char *id = ccb_sdk_toolchain_identity(&sdk, &cfg);
    CHECK_STR_EQ(id,
      "bt=35.0.0|aapt2=/sdk/build-tools/35.0.0/aapt2"
      "|zipalign=/sdk/build-tools/35.0.0/zipalign"
      "|kt=2.4.21|java=17|compile=35");
    free(id);
    // A kotlinc version change changes the identity.
    sdk.kotlin_version = "2.4.20";
    char *id2 = ccb_sdk_toolchain_identity(&sdk, &cfg);
    CHECK(strcmp(id2,
      "bt=35.0.0|aapt2=/sdk/build-tools/35.0.0/aapt2"
      "|zipalign=/sdk/build-tools/35.0.0/zipalign"
      "|kt=2.4.21|java=17|compile=35") != 0);
    free(id2);
    // Absent tools read as empty segments, never a crash: a
    // Java-only resolve leaves kotlinc/kotlin_version NULL.
    sdk.aapt2 = NULL;
    sdk.zipalign = NULL;
    sdk.kotlinc = NULL;
    sdk.kotlin_version = NULL;
    char *id3 = ccb_sdk_toolchain_identity(&sdk, &cfg);
    CHECK_STR_EQ(id3, "bt=35.0.0|kt=|java=17|compile=35");
    free(id3);
  }

  // --- shared fingerprint key groups ---
  {
    ccb_config cfg;
    ccb_sdk sdk;
    ccb_pipe p;
    memset(&cfg, 0, sizeof(cfg));
    memset(&sdk, 0, sizeof(sdk));
    memset(&p, 0, sizeof(p));
    p.cfg = &cfg;
    p.sdk = &sdk;
    p.cache = ccb_xcalloc(1, sizeof(ccb_cache));
    cfg.project_root = base;
    cfg.jvm_target = "17";
    sdk.build_tools_version = "35.0.0";
    sdk.aapt2 = "aapt2";
    sdk.kotlin_version = "2.4.21";
    ccb_fp_builder b;
    memset(&b, 0, sizeof(b));
    ccb_fp_add_aapt2_tools(&p, &b);
    CHECK(b.count == 2);
    CHECK(builder_has_key(&b, 0, "build_tools"));
    CHECK(builder_has_key(&b, 1, "aapt2"));
    CHECK_STR_EQ(b.vals[0], "35.0.0");
    CHECK_STR_EQ(b.vals[1], "aapt2");
    ccb_fp_add_kotlin_tools(&p, &b);
    CHECK(b.count == 4);
    CHECK(builder_has_key(&b, 2, "kotlinc"));
    CHECK(builder_has_key(&b, 3, "jvm_target"));
    CHECK_STR_EQ(b.vals[2], "2.4.21");
    CHECK_STR_EQ(b.vals[3], "17");
    ccb_fp_free(&b);
    // Declared jars: a missing jar is skipped when skip_missing,
    // fails the fingerprint when not.
    char *jars[] = { B("proj/j.jar"), B("proj/missing.jar") };
    p.resolved.jars = jars;
    p.resolved.jars_count = 2;
    int failed = 0;
    memset(&b, 0, sizeof(b));
    ccb_fp_add_declared_jars(&p, &b, &failed, 1);
    CHECK(failed == 0);
    CHECK(b.count == 1);
    CHECK(builder_has_key(&b, 0, "proj/j.jar"));
    ccb_fp_free(&b);
    memset(&b, 0, sizeof(b));
    ccb_fp_add_declared_jars(&p, &b, &failed, 0);
    CHECK(failed == 1);
    ccb_fp_free(&b);
    // A pre-set failure short-circuits: no keys are added.
    failed = 1;
    memset(&b, 0, sizeof(b));
    ccb_fp_add_declared_jars(&p, &b, &failed, 0);
    CHECK(b.count == 0);
    ccb_fp_free(&b);
    // AAR classes.jars follow the same rule.
    char *cls[] = { B("proj/k.jar"), B("proj/missing.jar") };
    p.aar_class_jars = cls;
    p.aar_class_jars_count = 2;
    failed = 0;
    memset(&b, 0, sizeof(b));
    ccb_fp_add_aar_class_jars(&p, &b, &failed, 1);
    CHECK(failed == 0);
    CHECK(b.count == 1);
    CHECK(builder_has_key(&b, 0, "proj/k.jar"));
    ccb_fp_free(&b);
    memset(&b, 0, sizeof(b));
    ccb_fp_add_aar_class_jars(&p, &b, &failed, 0);
    CHECK(failed == 1);
    ccb_fp_free(&b);
    ccb_cache_free(p.cache);
  }

  TEST_MAIN_END();
}
