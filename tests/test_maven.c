// Tests for remote Maven dependency resolution (ccb_maven).
// Fixture repositories live under /tmp/ccb_test_maven/repo in Maven
// layout and are reached through CCB_MAVEN_REPOS=file://... plus
// CCB_MAVEN_CACHE, so no test touches the network. The black-box
// end-to-end build needs a runnable SDK toolchain (same env fallback
// convention as test_aab.c) and skips gracefully without one.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_config.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_proc.h"
#include "ccb_str.h"
#include "ccb_zip.h"
#include <stdlib.h>
#include <unistd.h>

#define BASE "/tmp/ccb_test_maven"
#define REPO BASE "/repo"
#define CACHE BASE "/cache"

static void write_text(const char *path, const char *text) {
  CHECK(ccb_write_file(path, text, strlen(text)) == CCB_OK);
}

static void mkdir_or_fail(const char *path) {
  CHECK(ccb_mkdir_p(path) == CCB_OK);
}

static char *repo_leaf_path(const char *group, const char *artifact,
    const char *version, const char *leaf) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, REPO);
  ccb_str_append_c(&s, '/');
  for (const char *p = group; *p != '\0'; p++) {
    ccb_str_append_c(&s, *p == '.' ? '/' : *p);
  }
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, version);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, leaf);
  return ccb_str_steal(&s);
}

// Checksum modes for put_artifact.
#define CHK_NONE 0
#define CHK_SHA256 1
#define CHK_SHA1 2
#define CHK_WRONG 3

// Write one artifact + POM into the fixture repo. The artifact bytes
// are arbitrary for resolution tests; the e2e AAR is built separately
// with the zip writer.
static void put_artifact(const char *group, const char *artifact,
    const char *version, const char *ext, const char *pom, int chk) {
  ccb_str leaf;
  ccb_str_init(&leaf);
  ccb_str_append(&leaf, artifact);
  ccb_str_append_c(&leaf, '-');
  ccb_str_append(&leaf, version);
  ccb_str_append_c(&leaf, '.');
  ccb_str_append(&leaf, ext);
  char *art_path = repo_leaf_path(group, artifact, version, leaf.buf);
  ccb_str_free(&leaf);
  ccb_str pom_leaf;
  ccb_str_init(&pom_leaf);
  ccb_str_append(&pom_leaf, artifact);
  ccb_str_append_c(&pom_leaf, '-');
  ccb_str_append(&pom_leaf, version);
  ccb_str_append(&pom_leaf, ".pom");
  char *pom_path = repo_leaf_path(group, artifact, version, pom_leaf.buf);
  ccb_str_free(&pom_leaf);
  char *dir = ccb_dirname(art_path);
  CHECK(ccb_mkdir_p(dir) == CCB_OK);
  free(dir);
  write_text(pom_path, pom);
  if (strcmp(ext, "pom") == 0) {
    // For pom packaging the POM is the only file; there is no
    // separate artifact to write (it would share the same name).
    free(art_path);
    free(pom_path);
    return;
  }
  write_text(art_path, "fake-artifact-bytes");
  if (chk == CHK_SHA256 || chk == CHK_SHA1) {
    char hex[65];
    if (chk == CHK_SHA256) {
      CHECK(ccb_sha256_file(art_path, hex) == CCB_OK);
    } else {
      CHECK(ccb_sha1_file(art_path, hex) == CCB_OK);
    }
    ccb_str chk_path;
    ccb_str_init(&chk_path);
    ccb_str_append(&chk_path, art_path);
    ccb_str_append(&chk_path, chk == CHK_SHA256 ? ".sha256" : ".sha1");
    char *cp = ccb_str_steal(&chk_path);
    write_text(cp, hex);
    free(cp);
  } else if (chk == CHK_WRONG) {
    ccb_str chk_path;
    ccb_str_init(&chk_path);
    ccb_str_append(&chk_path, art_path);
    ccb_str_append(&chk_path, ".sha256");
    char *cp = ccb_str_steal(&chk_path);
    write_text(cp,
      "0000000000000000000000000000000000000000000000000000000000000000");
    free(cp);
  }
  free(art_path);
  free(pom_path);
}

static const char *POM_LIB =
  "<project><modelVersion>4.0.0</modelVersion>"
  "<groupId>com.example</groupId><artifactId>lib</artifactId>"
  "<version>1.0</version><packaging>aar</packaging>"
  "<dependencies>"
  "<dependency><groupId>com.example</groupId><artifactId>base</artifactId>"
  "<version>1.0</version><scope>compile</scope></dependency>"
  "<dependency><groupId>com.example</groupId><artifactId>testonly</artifactId>"
  "<version>1.0</version><scope>test</scope></dependency>"
  "<dependency><groupId>com.example</groupId><artifactId>opt</artifactId>"
  "<version>1.0</version><optional>true</optional></dependency>"
  "<dependency><groupId>com.example</groupId><artifactId>provided</artifactId>"
  "<version>1.0</version><scope>provided</scope></dependency>"
  "</dependencies></project>";
static const char *POM_BASE =
  "<project><modelVersion>4.0.0</modelVersion>"
  "<groupId>com.example</groupId><artifactId>base</artifactId>"
  "<version>1.0</version><packaging>jar</packaging>"
  "<dependencies>"
  "<dependency><groupId>com.example</groupId><artifactId>deep</artifactId>"
  "<version>2.0</version></dependency>"
  "</dependencies></project>";
static const char *POM_STANDALONE =
  "<project><modelVersion>4.0.0</modelVersion>"
  "<groupId>com.example</groupId><artifactId>standalone</artifactId>"
  "<version>1.0</version><packaging>jar</packaging></project>";

static void pom_simple(const char *group, const char *artifact,
    const char *version, const char *ext, const char *deps_xml) {
  ccb_str pom;
  ccb_str_init(&pom);
  ccb_str_append(&pom, "<project><modelVersion>4.0.0</modelVersion>");
  ccb_str_append(&pom, "<groupId>");
  ccb_str_append(&pom, group);
  ccb_str_append(&pom, "</groupId><artifactId>");
  ccb_str_append(&pom, artifact);
  ccb_str_append(&pom, "</artifactId><version>");
  ccb_str_append(&pom, version);
  ccb_str_append(&pom, "</version><packaging>");
  ccb_str_append(&pom, ext);
  ccb_str_append(&pom, "</packaging>");
  if (deps_xml != NULL) {
    ccb_str_append(&pom, "<dependencies>");
    ccb_str_append(&pom, deps_xml);
    ccb_str_append(&pom, "</dependencies>");
  }
  ccb_str_append(&pom, "</project>");
  put_artifact(group, artifact, version, ext, pom.buf, CHK_NONE);
  ccb_str_free(&pom);
}

static void dep_xml(ccb_str *s, const char *group, const char *artifact,
    const char *version) {
  ccb_str_append(s, "<dependency><groupId>");
  ccb_str_append(s, group);
  ccb_str_append(s, "</groupId><artifactId>");
  ccb_str_append(s, artifact);
  ccb_str_append(s, "</artifactId>");
  if (version != NULL) {
    ccb_str_append(s, "<version>");
    ccb_str_append(s, version);
    ccb_str_append(s, "</version>");
  }
  ccb_str_append(s, "</dependency>");
}

static int has_code(ccb_diag_list *diags, const char *code) {
  for (size_t i = 0; i < diags->count; i++) {
    if (strcmp(diags->items[i].code, code) == 0) {
      return 1;
    }
  }
  return 0;
}

static int result_has(ccb_maven_result *r, const char *leaf) {
  for (size_t i = 0; i < r->aars_count; i++) {
    const char *base = strrchr(r->aars[i], '/');
    if (base != NULL && strcmp(base + 1, leaf) == 0) {
      return 1;
    }
  }
  for (size_t i = 0; i < r->jars_count; i++) {
    const char *base = strrchr(r->jars[i], '/');
    if (base != NULL && strcmp(base + 1, leaf) == 0) {
      return 1;
    }
  }
  return 0;
}

// Build a manual config carrying exactly the given coordinates.
static ccb_config *manual_cfg(const char **coords, size_t count) {
  ccb_config *c = ccb_xcalloc(1, sizeof(*c));
  c->maven = ccb_xmalloc((count == 0 ? 1 : count) * sizeof(*c->maven));
  for (size_t i = 0; i < count; i++) {
    c->maven[i] = ccb_xstrdup(coords[i]);
  }
  c->maven_count = count;
  return c;
}

static ccb_status resolve_here(const char **coords, size_t count, int prefetch,
    ccb_maven_result *out, ccb_diag_list *diags) {
  ccb_config *c = manual_cfg(coords, count);
  ccb_status rc = ccb_maven_resolve(c, prefetch, 0, 0, out, diags);
  ccb_config_free(c);
  return rc;
}

static void test_coords(void) {
  char *g = NULL;
  char *a = NULL;
  char *v = NULL;
  CHECK(ccb_maven_parse_coord("com.example:lib:1.0", &g, &a, &v));
  CHECK_STR_EQ(g, "com.example");
  CHECK_STR_EQ(a, "lib");
  CHECK_STR_EQ(v, "1.0");
  free(g);
  free(a);
  free(v);
  g = NULL;
  a = NULL;
  v = NULL;
  CHECK(!ccb_maven_parse_coord("com.example:lib", &g, &a, &v));
  CHECK(!ccb_maven_parse_coord("com.example:lib:1.0:extra", &g, &a, &v));
  CHECK(!ccb_maven_parse_coord("com.example::1.0", &g, &a, &v));
  CHECK(!ccb_maven_parse_coord("com.example:lib:", &g, &a, &v));
  CHECK(!ccb_maven_parse_coord("com/example:lib:1.0", &g, &a, &v));
  CHECK(!ccb_maven_parse_coord("com.example:li/b:1.0", &g, &a, &v));
  CHECK(!ccb_maven_parse_coord("com.example:lib:../evil", &g, &a, &v));
  CHECK(g == NULL && a == NULL && v == NULL);
}

static void test_sha1(void) {
  // FIPS 180-1 vectors, guarding the in-tree SHA-1 used for .sha1
  // checksum verification.
  char hex[41];
  ccb_sha1 c;
  unsigned char d[20];
  ccb_sha1_init(&c);
  ccb_sha1_update(&c, "abc", 3);
  ccb_sha1_final(&c, d);
  ccb_sha1_to_hex(d, hex);
  CHECK_STR_EQ(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");
  ccb_sha1_init(&c);
  ccb_sha1_final(&c, d);
  ccb_sha1_to_hex(d, hex);
  CHECK_STR_EQ(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  ccb_sha1_init(&c);
  ccb_sha1_update(&c, m, strlen(m));
  ccb_sha1_final(&c, d);
  ccb_sha1_to_hex(d, hex);
  CHECK_STR_EQ(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

static void build_repo(void) {
  put_artifact("com.example", "lib", "1.0", "aar", POM_LIB, CHK_NONE);
  put_artifact("com.example", "base", "1.0", "jar", POM_BASE, CHK_NONE);
  put_artifact("com.example", "standalone", "1.0", "jar", POM_STANDALONE,
    CHK_NONE);
  pom_simple("com.example", "deep", "1.0", "jar", NULL);
  pom_simple("com.example", "standalone", "9.9", "jar", NULL);
  pom_simple("com.example", "deep", "2.0", "jar", NULL);
  pom_simple("com.example", "deep", "3.0", "jar", NULL);
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "deep", "2.0");
    pom_simple("com.example", "a", "1.0", "jar", deps.buf);
    ccb_str_free(&deps);
  }
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "deep", "3.0");
    pom_simple("com.example", "b", "1.0", "jar", deps.buf);
    ccb_str_free(&deps);
  }
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "cyc2", "1.0");
    pom_simple("com.example", "cyc1", "1.0", "jar", deps.buf);
    ccb_str_free(&deps);
  }
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "cyc1", "1.0");
    pom_simple("com.example", "cyc2", "1.0", "jar", deps.buf);
    ccb_str_free(&deps);
  }
  // Properties: dependency version from ${ver}.
  put_artifact("com.example", "prop", "1.0", "jar",
    "<project><modelVersion>4.0.0</modelVersion>"
    "<groupId>com.example</groupId><artifactId>prop</artifactId>"
    "<version>1.0</version><packaging>jar</packaging>"
    "<properties><ver>1.0</ver></properties>"
    "<dependencies><dependency><groupId>com.example</groupId>"
    "<artifactId>standalone</artifactId><version>${ver}</version>"
    "</dependency></dependencies></project>",
    CHK_NONE);
  // dependencyManagement: version-less dependency + managed version.
  put_artifact("com.example", "mgmt", "1.0", "jar",
    "<project><modelVersion>4.0.0</modelVersion>"
    "<groupId>com.example</groupId><artifactId>mgmt</artifactId>"
    "<version>1.0</version><packaging>jar</packaging>"
    "<dependencyManagement><dependencies><dependency>"
    "<groupId>com.example</groupId><artifactId>standalone</artifactId>"
    "<version>1.0</version></dependency></dependencies>"
    "</dependencyManagement>"
    "<dependencies><dependency><groupId>com.example</groupId>"
    "<artifactId>standalone</artifactId></dependency></dependencies></project>",
    CHK_NONE);
  // Parent inheritance: no own groupId/version, parent block provides.
  put_artifact("com.example", "child", "1.0", "jar",
    "<project><modelVersion>4.0.0</modelVersion>"
    "<parent><groupId>com.example</groupId><artifactId>parent</artifactId>"
    "<version>9.9</version></parent>"
    "<artifactId>child</artifactId><packaging>jar</packaging>"
    "<dependencies><dependency><groupId>com.example</groupId>"
    "<artifactId>standalone</artifactId><version>${project.version}</version>"
    "</dependency></dependencies></project>",
    CHK_NONE);
  // Packaging pom: no artifact of its own, but its dependency resolves.
  put_artifact("com.example", "bom", "1.0", "pom",
    "<project><modelVersion>4.0.0</modelVersion>"
    "<groupId>com.example</groupId><artifactId>bom</artifactId>"
    "<version>1.0</version><packaging>pom</packaging>"
    "<dependencies><dependency><groupId>com.example</groupId>"
    "<artifactId>standalone</artifactId><version>1.0</version>"
    "</dependency></dependencies></project>",
    CHK_NONE);
  // Unsupported packaging.
  pom_simple("com.example", "warlib", "1.0", "war", NULL);
  // Dependency without a version and no management entry.
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "standalone", NULL);
    pom_simple("com.example", "noversion", "1.0", "jar", deps.buf);
    ccb_str_free(&deps);
  }
  // Dependency version with an unterminated ${ reference.
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "standalone", "1.0${unclosed");
    pom_simple("com.example", "badprop", "1.0", "jar", deps.buf);
    ccb_str_free(&deps);
  }
  // Checksum cases.
  pom_simple("com.example", "goodsha", "1.0", "jar", NULL);
  {
    // Re-put goodsha with a correct .sha1 (pom_simple wrote CHK_NONE).
    ccb_str leaf;
    ccb_str_init(&leaf);
    ccb_str_append(&leaf, "goodsha-1.0.jar");
    char *art_path = repo_leaf_path("com.example", "goodsha", "1.0", leaf.buf);
    ccb_str_free(&leaf);
    char hex[41];
    CHECK(ccb_sha1_file(art_path, hex) == CCB_OK);
    ccb_str cp;
    ccb_str_init(&cp);
    ccb_str_append(&cp, art_path);
    ccb_str_append(&cp, ".sha1");
    char *chk = ccb_str_steal(&cp);
    write_text(chk, hex);
    free(chk);
    free(art_path);
  }
  put_artifact("com.example", "badsha", "1.0", "jar",
    "<project><modelVersion>4.0.0</modelVersion>"
    "<groupId>com.example</groupId><artifactId>badsha</artifactId>"
    "<version>1.0</version><packaging>jar</packaging></project>",
    CHK_WRONG);
}

static void test_direct_and_scopes(void) {
  const char *coords[] = { "com.example:lib:1.0", "com.example:standalone:1.0" };
  ccb_maven_result out;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  CHECK(resolve_here(coords, 2, 1, &out, &diags) == CCB_OK);
  CHECK(out.aars_count == 1);
  CHECK(out.jars_count == 3);
  CHECK(result_has(&out, "lib-1.0.aar"));
  CHECK(result_has(&out, "base-1.0.jar"));
  CHECK(result_has(&out, "standalone-1.0.jar"));
  // Skipped scopes/optional never resolve: their artifacts do not
  // exist in the repo, so any fetch attempt would have failed.
  CHECK(!result_has(&out, "testonly-1.0.jar"));
  CHECK(!result_has(&out, "opt-1.0.jar"));
  CHECK(!result_has(&out, "provided-1.0.jar"));
  // The transitive deep:2.0 (via base) is present here.
  CHECK(result_has(&out, "deep-2.0.jar"));
  // Sidecars recorded for checksum-less downloads.
  ccb_str side;
  ccb_str_init(&side);
  ccb_str_append(&side, CACHE);
  ccb_str_append(&side, "/com/example/lib/1.0/lib-1.0.aar.ccb-sha256");
  CHECK(ccb_path_exists(side.buf));
  ccb_str_free(&side);
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
}

static void test_nearest_wins(void) {
  // Direct deep:1.0 (depth 0) beats transitive deep:2.0 (depth 2).
  const char *coords[] = { "com.example:lib:1.0", "com.example:deep:1.0" };
  ccb_maven_result out;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  CHECK(resolve_here(coords, 2, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "deep-1.0.jar"));
  CHECK(!result_has(&out, "deep-2.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  // Same-depth conflict: first-declared (a -> deep:2.0) wins over
  // b -> deep:3.0.
  const char *coords2[] = { "com.example:a:1.0", "com.example:b:1.0" };
  CHECK(resolve_here(coords2, 2, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "deep-2.0.jar"));
  CHECK(!result_has(&out, "deep-3.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
}

static void test_cycle_props_mgmt_parent_bom(void) {
  ccb_maven_result out;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  const char *cyc[] = { "com.example:cyc1:1.0" };
  CHECK(resolve_here(cyc, 1, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "cyc1-1.0.jar"));
  CHECK(result_has(&out, "cyc2-1.0.jar"));
  ccb_maven_result_free(&out);
  const char *prop[] = { "com.example:prop:1.0" };
  CHECK(resolve_here(prop, 1, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "standalone-1.0.jar"));
  ccb_maven_result_free(&out);
  const char *mgmt[] = { "com.example:mgmt:1.0" };
  CHECK(resolve_here(mgmt, 1, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "standalone-1.0.jar"));
  ccb_maven_result_free(&out);
  const char *child[] = { "com.example:child:1.0" };
  CHECK(resolve_here(child, 1, 1, &out, &diags) == CCB_OK);
  // The dependency version is ${project.version}: the child's own
  // version comes from the parent block (9.9), so standalone:9.9 wins.
  CHECK(result_has(&out, "standalone-9.9.jar"));
  ccb_maven_result_free(&out);
  const char *bom[] = { "com.example:bom:1.0" };
  CHECK(resolve_here(bom, 1, 1, &out, &diags) == CCB_OK);
  CHECK(out.aars_count == 0 && out.jars_count == 1);
  CHECK(result_has(&out, "standalone-1.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
}

static void test_failures(void) {
  ccb_maven_result out;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  const char *war[] = { "com.example:warlib:1.0" };
  CHECK(resolve_here(war, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3019"));
  ccb_diag_list_free(&diags);
  ccb_diag_list_init(&diags);
  const char *nv[] = { "com.example:noversion:1.0" };
  CHECK(resolve_here(nv, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3019"));
  ccb_diag_list_free(&diags);
  ccb_diag_list_init(&diags);
  const char *bp[] = { "com.example:badprop:1.0" };
  CHECK(resolve_here(bp, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3019"));
  ccb_diag_list_free(&diags);
  ccb_diag_list_init(&diags);
  const char *missing[] = { "com.example:nope:1.0" };
  CHECK(resolve_here(missing, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3019"));
  ccb_diag_list_free(&diags);
  ccb_diag_list_init(&diags);
  const char *bad[] = { "com.example:badsha:1.0" };
  CHECK(resolve_here(bad, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3020"));
  ccb_diag_list_free(&diags);
  ccb_diag_list_init(&diags);
  const char *good[] = { "com.example:goodsha:1.0" };
  CHECK(resolve_here(good, 1, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "goodsha-1.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
}

static void test_cache_only(void) {
  // Fresh cache, cache-only mode: every coordinate misses.
  char *cache2 = ccb_path_join(BASE, "cache2");
  CHECK(ccb_mkdir_p(cache2) == CCB_OK);
  const char *old = getenv("CCB_MAVEN_CACHE");
  char *saved = old != NULL ? ccb_xstrdup(old) : NULL;
  setenv("CCB_MAVEN_CACHE", cache2, 1);
  ccb_maven_result out;
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  const char *coords[] = { "com.example:standalone:1.0" };
  CHECK(resolve_here(coords, 1, 0, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3021"));
  ccb_diag_list_free(&diags);
  // Prefetch, delete the repo, then cache-only still succeeds: the
  // build path never touches the network.
  ccb_diag_list_init(&diags);
  CHECK(resolve_here(coords, 1, 1, &out, &diags) == CCB_OK);
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  ccb_remove_recursive(REPO);
  ccb_diag_list_init(&diags);
  CHECK(resolve_here(coords, 1, 0, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "standalone-1.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  if (saved != NULL) {
    setenv("CCB_MAVEN_CACHE", saved, 1);
    free(saved);
  } else {
    unsetenv("CCB_MAVEN_CACHE");
  }
  free(cache2);
}

// ---------- config-load validation ----------

#define PROJ_CFG BASE "/proj_cfg"

static void write_cfg_toml(const char *coord) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s,
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"hello\"\n"
    "package = \"com.example.hello\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n"
    "\n"
    "[deps]\n"
    "maven = [\"");
  ccb_str_append(&s, coord);
  ccb_str_append(&s, "\"]\n");
  char *toml = ccb_str_steal(&s);
  write_text(PROJ_CFG "/ccb.toml", toml);
  free(toml);
}

static void test_config_validation(void) {
  mkdir_or_fail(PROJ_CFG "/src/main/res/values");
  mkdir_or_fail(PROJ_CFG "/src/main/kotlin");
  write_text(PROJ_CFG "/src/main/AndroidManifest.xml",
    "<manifest package=\"com.example.hello\" />\n");
  static const char *bad[] = {
    "com.example:lib",
    "com.example:lib:1.0:extra",
    "com.example:lib:[1.0,2.0)",
    "com.example:lib:1.0-SNAPSHOT",
    "com.example:lib:LATEST",
    "com.example:lib:RELEASE",
    "com.example:lib:",
    "com.example::1.0",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    write_cfg_toml(bad[i]);
    ccb_config *cfg = NULL;
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(PROJ_CFG "/ccb.toml", NULL, &cfg, &diags) != CCB_OK);
    CHECK(cfg == NULL);
    CHECK(has_code(&diags, "CCB-E1005"));
    ccb_diag_list_free(&diags);
  }
  write_cfg_toml("com.example:lib:1.0");
  {
    ccb_config *cfg = NULL;
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(PROJ_CFG "/ccb.toml", NULL, &cfg, &diags) == CCB_OK);
    CHECK(cfg != NULL && cfg->maven_count == 1);
    CHECK_STR_EQ(cfg->maven[0], "com.example:lib:1.0");
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
  }
}

// ---------- end-to-end build ----------

#define REPO_E2E BASE "/repo_e2e"
#define CACHE_E2E BASE "/cache_e2e"
#define PROJ_E2E BASE "/proj_e2e"
#define APK_E2E PROJ_E2E "/build/outputs/debug/app.apk"

static char *find_binary(int argc, char **argv) {
  if (argc > 1 && argv[1] != NULL && access(argv[1], X_OK) == 0) {
    char resolved[4096];
    if (realpath(argv[1], resolved) != NULL) {
      return ccb_xstrdup(resolved);
    }
    return ccb_xstrdup(argv[1]);
  }
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
    char *dir = ccb_dirname(exe);
    char *cand = ccb_path_join(dir, "ccb");
    free(dir);
    if (access(cand, X_OK) == 0) {
      return cand;
    }
    free(cand);
  }
  return NULL;
}

static char *tool_home(const char *env_name, const char *fallback) {
  const char *v = getenv(env_name);
  if (v != NULL && v[0] != '\0') {
    return ccb_xstrdup(v);
  }
  const char *home = getenv("HOME");
  if (home == NULL) {
    return NULL;
  }
  return ccb_path_join(home, fallback);
}

static void build_e2e_repo(void) {
  // A real res-only AAR (no classes.jar needed): manifest +
  // resources, which the deps step extracts and the link step merges.
  ccb_str d;
  ccb_str_init(&d);
  ccb_str_append(&d, REPO_E2E);
  ccb_str_append(&d, "/com/example/lib/1.0");
  char *libdir = ccb_str_steal(&d);
  CHECK(ccb_mkdir_p(libdir) == CCB_OK);
  ccb_str aar;
  ccb_str_init(&aar);
  ccb_str_append(&aar, libdir);
  ccb_str_append(&aar, "/lib-1.0.aar");
  char *aar_path = ccb_str_steal(&aar);
  ccb_zip_writer w;
  CHECK(ccb_zip_writer_init(&w, aar_path) == CCB_OK);
  const char *lib_manifest =
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "    package=\"com.example.lib\">\n"
    "    <uses-permission android:name=\"android.permission.INTERNET\" />\n"
    "    <application />\n"
    "</manifest>\n";
  const char *lib_strings =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"lib_hello\">Hello from the library!</string>\n"
    "</resources>\n";
  CHECK(ccb_zip_writer_add_data(&w, "AndroidManifest.xml", lib_manifest,
    strlen(lib_manifest)) == CCB_OK);
  CHECK(ccb_zip_writer_add_data(&w, "res/values/strings.xml", lib_strings,
    strlen(lib_strings)) == CCB_OK);
  CHECK(ccb_zip_writer_finish(&w) == CCB_OK);
  ccb_str pom;
  ccb_str_init(&pom);
  ccb_str_append(&pom, libdir);
  ccb_str_append(&pom, "/lib-1.0.pom");
  char *pom_path = ccb_str_steal(&pom);
  write_text(pom_path,
    "<project><modelVersion>4.0.0</modelVersion>"
    "<groupId>com.example</groupId><artifactId>lib</artifactId>"
    "<version>1.0</version><packaging>aar</packaging></project>");
  free(pom_path);
  free(aar_path);
  free(libdir);
}

static void write_e2e_project(void) {
  mkdir_or_fail(PROJ_E2E "/src/main/kotlin/com/example/hello");
  mkdir_or_fail(PROJ_E2E "/src/main/res/values");
  mkdir_or_fail(PROJ_E2E "/src/main/res/layout");
  write_text(PROJ_E2E "/ccb.toml",
    "ccb_version = 1\n"
    "\n"
    "[project]\n"
    "name = \"hello\"\n"
    "package = \"com.example.hello\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n"
    "\n"
    "[deps]\n"
    "maven = [\"com.example:lib:1.0\"]\n");
  write_text(PROJ_E2E "/src/main/AndroidManifest.xml",
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"com.example.hello\">\n"
    "  <application android:label=\"@string/app_name\" android:theme=\"@style/AppTheme\">\n"
    "    <activity android:name=\".MainActivity\" android:exported=\"true\">\n"
    "      <intent-filter>\n"
    "        <action android:name=\"android.intent.action.MAIN\" />\n"
    "        <category android:name=\"android.intent.category.LAUNCHER\" />\n"
    "      </intent-filter>\n"
    "    </activity>\n"
    "  </application>\n"
    "</manifest>\n");
  write_text(PROJ_E2E "/src/main/kotlin/com/example/hello/MainActivity.kt",
    "package com.example.hello\n"
    "\n"
    "import android.app.Activity\n"
    "import android.os.Bundle\n"
    "\n"
    "class MainActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    setContentView(R.layout.activity_main)\n"
    "  }\n"
    "}\n");
  write_text(PROJ_E2E "/src/main/res/values/strings.xml",
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"app_name\">hello</string>\n"
    "</resources>\n");
  write_text(PROJ_E2E "/src/main/res/values/themes.xml",
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <style name=\"AppTheme\" parent=\"android:Theme.Material\" />\n"
    "</resources>\n");
  write_text(PROJ_E2E "/src/main/res/layout/activity_main.xml",
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\">\n"
    "</LinearLayout>\n");
}

static void test_e2e(int argc, char **argv) {
  char *bin = find_binary(argc, argv);
  char *sdk_home = tool_home("ANDROID_HOME", "android-sdk");
  char *java_home = tool_home("JAVA_HOME", "jdk17");
  char *kotlin_home = tool_home("KOTLIN_HOME", "kotlin-2.4.21");
  char *aapt2 = sdk_home == NULL ? NULL
    : ccb_path_join3(sdk_home, "build-tools/35.0.0", "aapt2");
  char *jarsigner = java_home == NULL ? NULL : ccb_path_join(java_home, "bin/jarsigner");
  if (bin == NULL || aapt2 == NULL || access(aapt2, X_OK) != 0 ||
      jarsigner == NULL || access(jarsigner, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping e2e build checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(aapt2);
    free(jarsigner);
    return;
  }
  build_e2e_repo();
  write_e2e_project();
  // Prefetch in-process (setup itself cannot run on this VM: its
  // sdkmanager step has no direct repository access).
  const char *old_repos = getenv("CCB_MAVEN_REPOS");
  const char *old_cache = getenv("CCB_MAVEN_CACHE");
  char *saved_repos = old_repos != NULL ? ccb_xstrdup(old_repos) : NULL;
  char *saved_cache = old_cache != NULL ? ccb_xstrdup(old_cache) : NULL;
  {
    ccb_str r;
    ccb_str_init(&r);
    ccb_str_append(&r, "file://");
    ccb_str_append(&r, REPO_E2E);
    char *repos_env = ccb_str_steal(&r);
    setenv("CCB_MAVEN_REPOS", repos_env, 1);
    free(repos_env);
    setenv("CCB_MAVEN_CACHE", CACHE_E2E, 1);
  }
  {
    const char *coords[] = { "com.example:lib:1.0" };
    ccb_maven_result out;
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    CHECK(resolve_here(coords, 1, 1, &out, &diags) == CCB_OK);
    CHECK(result_has(&out, "lib-1.0.aar"));
    ccb_maven_result_free(&out);
    ccb_diag_list_free(&diags);
  }
  // Child environment for build/check runs.
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "ANDROID_HOME=");
  ccb_str_append(&s, sdk_home);
  char *sdk_env = ccb_str_steal(&s);
  ccb_str_init(&s);
  ccb_str_append(&s, "JAVA_HOME=");
  ccb_str_append(&s, java_home);
  char *java_env = ccb_str_steal(&s);
  ccb_str_init(&s);
  ccb_str_append(&s, "KOTLIN_HOME=");
  ccb_str_append(&s, kotlin_home);
  char *kotlin_env = ccb_str_steal(&s);
  ccb_str_init(&s);
  ccb_str_append(&s, "CCB_MAVEN_CACHE=");
  ccb_str_append(&s, CACHE_E2E);
  char *cache_env = ccb_str_steal(&s);
  ccb_str_init(&s);
  ccb_str_append(&s, "HOME=");
  ccb_str_append(&s, BASE "/home");
  char *home_env = ccb_str_steal(&s);
  char *const env_extra[] = { sdk_env, java_env, kotlin_env, cache_env,
    home_env, NULL };
  {
    char *args[] = { bin, "check", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ_E2E, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "maven:") != NULL);
    ccb_proc_result_free(&r);
  }
  {
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ_E2E, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(ccb_path_exists(APK_E2E));
    ccb_proc_result_free(&r);
  }
  // Negative control: an empty cache makes the same build fail with
  // CCB-E3021 before any stage runs.
  {
    char *empty_cache = ccb_path_join(BASE, "cache_empty");
    CHECK(ccb_mkdir_p(empty_cache) == CCB_OK);
    ccb_str ce;
    ccb_str_init(&ce);
    ccb_str_append(&ce, "CCB_MAVEN_CACHE=");
    ccb_str_append(&ce, empty_cache);
    char *cache_empty_env = ccb_str_steal(&ce);
    char *const env_empty[] = { sdk_env, java_env, kotlin_env,
      cache_empty_env, home_env, NULL };
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ_E2E, env_empty, &r) == CCB_OK);
    CHECK(r.exit_code != 0);
    CHECK(strstr(r.err, "CCB-E3021") != NULL);
    ccb_proc_result_free(&r);
    free(cache_empty_env);
    free(empty_cache);
  }
  if (saved_repos != NULL) {
    setenv("CCB_MAVEN_REPOS", saved_repos, 1);
    free(saved_repos);
  } else {
    unsetenv("CCB_MAVEN_REPOS");
  }
  if (saved_cache != NULL) {
    setenv("CCB_MAVEN_CACHE", saved_cache, 1);
    free(saved_cache);
  } else {
    unsetenv("CCB_MAVEN_CACHE");
  }
  free(sdk_env);
  free(java_env);
  free(kotlin_env);
  free(cache_env);
  free(home_env);
  free(bin);
  free(sdk_home);
  free(java_home);
  free(kotlin_home);
  free(aapt2);
  free(jarsigner);
}

int main(int argc, char **argv) {
  const char *old_repos = getenv("CCB_MAVEN_REPOS");
  const char *old_cache = getenv("CCB_MAVEN_CACHE");
  char *saved_repos = old_repos != NULL ? ccb_xstrdup(old_repos) : NULL;
  char *saved_cache = old_cache != NULL ? ccb_xstrdup(old_cache) : NULL;
  ccb_remove_recursive(BASE);
  CHECK(ccb_mkdir_p(BASE) == CCB_OK);
  {
    ccb_str r;
    ccb_str_init(&r);
    ccb_str_append(&r, "file://");
    ccb_str_append(&r, REPO);
    char *repos_env = ccb_str_steal(&r);
    setenv("CCB_MAVEN_REPOS", repos_env, 1);
    free(repos_env);
    setenv("CCB_MAVEN_CACHE", CACHE, 1);
  }
  test_coords();
  test_sha1();
  build_repo();
  test_direct_and_scopes();
  test_nearest_wins();
  test_cycle_props_mgmt_parent_bom();
  test_failures();
  test_cache_only();
  test_config_validation();
  test_e2e(argc, argv);
  if (saved_repos != NULL) {
    setenv("CCB_MAVEN_REPOS", saved_repos, 1);
    free(saved_repos);
  } else {
    unsetenv("CCB_MAVEN_REPOS");
  }
  if (saved_cache != NULL) {
    setenv("CCB_MAVEN_CACHE", saved_cache, 1);
    free(saved_cache);
  } else {
    unsetenv("CCB_MAVEN_CACHE");
  }
  TEST_MAIN_END();
}
