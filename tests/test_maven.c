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
  // The child's parent POM is fetched during resolution. It carries
  // no properties or management, so inheritance behaves as before.
  put_artifact("com.example", "parent", "9.9", "pom",
    "<project><modelVersion>4.0.0</modelVersion>"
    "<groupId>com.example</groupId><artifactId>parent</artifactId>"
    "<version>9.9</version><packaging>pom</packaging></project>",
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

// ---------- parent POMs ----------

#define REPO_P BASE "/repo_parents"
#define CACHE_P BASE "/cache_parents"

// put_artifact rooted at REPO_P (same layout, no published checksums:
// the parents tests never need them).
static void put_p(const char *group, const char *artifact,
    const char *version, const char *ext, const char *pom) {
  ccb_str dir;
  ccb_str_init(&dir);
  ccb_str_append(&dir, REPO_P);
  ccb_str_append_c(&dir, '/');
  for (const char *p = group; *p != '\0'; p++) {
    ccb_str_append_c(&dir, *p == '.' ? '/' : *p);
  }
  ccb_str_append_c(&dir, '/');
  ccb_str_append(&dir, artifact);
  ccb_str_append_c(&dir, '/');
  ccb_str_append(&dir, version);
  char *d = ccb_str_steal(&dir);
  CHECK(ccb_mkdir_p(d) == CCB_OK);
  ccb_str pom_path;
  ccb_str_init(&pom_path);
  ccb_str_append(&pom_path, d);
  ccb_str_append_c(&pom_path, '/');
  ccb_str_append(&pom_path, artifact);
  ccb_str_append_c(&pom_path, '-');
  ccb_str_append(&pom_path, version);
  ccb_str_append(&pom_path, ".pom");
  char *pp = ccb_str_steal(&pom_path);
  write_text(pp, pom);
  free(pp);
  if (strcmp(ext, "pom") != 0) {
    ccb_str art;
    ccb_str_init(&art);
    ccb_str_append(&art, d);
    ccb_str_append_c(&art, '/');
    ccb_str_append(&art, artifact);
    ccb_str_append_c(&art, '-');
    ccb_str_append(&art, version);
    ccb_str_append_c(&art, '.');
    ccb_str_append(&art, ext);
    char *ap = ccb_str_steal(&art);
    write_text(ap, "fake-artifact-bytes");
    free(ap);
  }
  free(d);
}

// Build a POM document from optional sections (NULL = absent) and
// publish it into the parents repo. The caller frees nothing: the
// built document is written and freed here.
static void put_built(const char *group, const char *artifact,
    const char *version, const char *packaging, const char *parent_xml,
    const char *props_xml, const char *mgmt_xml, const char *deps_xml) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "<project><modelVersion>4.0.0</modelVersion>");
  if (parent_xml != NULL) {
    ccb_str_append(&s, parent_xml);
  }
  ccb_str_append(&s, "<groupId>");
  ccb_str_append(&s, group);
  ccb_str_append(&s, "</groupId><artifactId>");
  ccb_str_append(&s, artifact);
  ccb_str_append(&s, "</artifactId><version>");
  ccb_str_append(&s, version);
  ccb_str_append(&s, "</version><packaging>");
  ccb_str_append(&s, packaging);
  ccb_str_append(&s, "</packaging>");
  if (props_xml != NULL) {
    ccb_str_append(&s, "<properties>");
    ccb_str_append(&s, props_xml);
    ccb_str_append(&s, "</properties>");
  }
  if (mgmt_xml != NULL) {
    ccb_str_append(&s, "<dependencyManagement><dependencies>");
    ccb_str_append(&s, mgmt_xml);
    ccb_str_append(&s, "</dependencies></dependencyManagement>");
  }
  if (deps_xml != NULL) {
    ccb_str_append(&s, "<dependencies>");
    ccb_str_append(&s, deps_xml);
    ccb_str_append(&s, "</dependencies>");
  }
  ccb_str_append(&s, "</project>");
  char *pom = ccb_str_steal(&s);
  put_p(group, artifact, version, packaging, pom);
  free(pom);
}

static char *parent_xml_p(const char *group, const char *artifact,
    const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "<parent><groupId>");
  ccb_str_append(&s, group);
  ccb_str_append(&s, "</groupId><artifactId>");
  ccb_str_append(&s, artifact);
  ccb_str_append(&s, "</artifactId><version>");
  ccb_str_append(&s, version);
  ccb_str_append(&s, "</version></parent>");
  return ccb_str_steal(&s);
}

static int diag_msg_has(ccb_diag_list *diags, const char *substr) {
  for (size_t i = 0; i < diags->count; i++) {
    if (diags->items[i].message != NULL &&
        strstr(diags->items[i].message, substr) != NULL) {
      return 1;
    }
  }
  return 0;
}

static void build_repo_parents(void) {
  // A: two-level chain, grandparent properties + management.
  put_built("com.example", "gp", "1.0", "pom", NULL,
    "<libver>2.0</libver>",
    "<dependency><groupId>com.example</groupId><artifactId>standalone</artifactId>"
    "<version>${libver}</version></dependency>",
    NULL);
  {
    char *par = parent_xml_p("com.example", "gp", "1.0");
    put_built("com.example", "mid", "1.0", "pom", par, NULL, NULL, NULL);
    free(par);
  }
  {
    char *par = parent_xml_p("com.example", "mid", "1.0");
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "standalone", NULL);
    put_built("com.example", "app", "1.0", "jar", par, NULL, NULL, deps.buf);
    ccb_str_free(&deps);
    free(par);
  }
  // B: child property overrides the parent's property.
  put_built("com.example", "parb", "1.0", "pom", NULL,
    "<v>1.0</v>",
    "<dependency><groupId>com.example</groupId><artifactId>standalone</artifactId>"
    "<version>${v}</version></dependency>",
    NULL);
  {
    char *par = parent_xml_p("com.example", "parb", "1.0");
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "standalone", NULL);
    put_built("com.example", "appb", "1.0", "jar", par, "<v>9.9</v>", NULL,
      deps.buf);
    ccb_str_free(&deps);
    free(par);
  }
  // C: child dependencyManagement overrides the parent's.
  put_built("com.example", "parc", "1.0", "pom", NULL, NULL,
    "<dependency><groupId>com.example</groupId><artifactId>standalone</artifactId>"
    "<version>1.0</version></dependency>",
    NULL);
  {
    char *par = parent_xml_p("com.example", "parc", "1.0");
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "standalone", NULL);
    put_built("com.example", "appc", "1.0", "jar", par, NULL,
      "<dependency><groupId>com.example</groupId><artifactId>standalone</artifactId>"
      "<version>2.0</version></dependency>",
      deps.buf);
    ccb_str_free(&deps);
    free(par);
  }
  // D: parent cycle.
  {
    char *par = parent_xml_p("com.example", "cyb", "1.0");
    put_built("com.example", "cya", "1.0", "pom", par, NULL, NULL, NULL);
    free(par);
  }
  {
    char *par = parent_xml_p("com.example", "cya", "1.0");
    put_built("com.example", "cyb", "1.0", "pom", par, NULL, NULL, NULL);
    free(par);
  }
  // E: missing parent (prefetch) and cached-parent eviction
  // (cache-only). pare exists in the repo; missing does not.
  {
    char *par = parent_xml_p("com.example", "pare", "1.0");
    put_built("com.example", "appe", "1.0", "jar", par, NULL, NULL, NULL);
    free(par);
  }
  put_built("com.example", "pare", "1.0", "pom", NULL, NULL, NULL, NULL);
  {
    char *par = parent_xml_p("com.example", "missing", "1.0");
    put_built("com.example", "appf", "1.0", "jar", par, NULL, NULL, NULL);
    free(par);
  }
  // F: depth chains, generated: d0 -> ... -> d11 (11 parents, over
  // the cap) and e0 -> ... -> e10 (exactly 10 parents, allowed).
  for (int i = 0; i <= 11; i++) {
    char art[16];
    snprintf(art, sizeof(art), "d%d", i);
    char *par = NULL;
    if (i < 11) {
      char next[16];
      snprintf(next, sizeof(next), "d%d", i + 1);
      par = parent_xml_p("com.example", next, "1.0");
    }
    put_built("com.example", art, "1.0", "pom", par, NULL, NULL, NULL);
    free(par);
  }
  for (int i = 0; i <= 10; i++) {
    char art[16];
    snprintf(art, sizeof(art), "e%d", i);
    char *par = NULL;
    if (i < 10) {
      char next[16];
      snprintf(next, sizeof(next), "e%d", i + 1);
      par = parent_xml_p("com.example", next, "1.0");
    }
    put_built("com.example", art, "1.0", "pom", par, NULL, NULL, NULL);
    free(par);
  }
  // Standalone versions the scenarios resolve to.
  put_built("com.example", "standalone", "1.0", "jar", NULL, NULL, NULL,
    NULL);
  put_built("com.example", "standalone", "2.0", "jar", NULL, NULL, NULL,
    NULL);
  put_built("com.example", "standalone", "9.9", "jar", NULL, NULL, NULL,
    NULL);
}

static void test_parents(void) {
  const char *old_repos = getenv("CCB_MAVEN_REPOS");
  const char *old_cache = getenv("CCB_MAVEN_CACHE");
  char *saved_repos = old_repos != NULL ? ccb_xstrdup(old_repos) : NULL;
  char *saved_cache = old_cache != NULL ? ccb_xstrdup(old_cache) : NULL;
  {
    ccb_str r;
    ccb_str_init(&r);
    ccb_str_append(&r, "file://");
    ccb_str_append(&r, REPO_P);
    char *repos_env = ccb_str_steal(&r);
    setenv("CCB_MAVEN_REPOS", repos_env, 1);
    free(repos_env);
    setenv("CCB_MAVEN_CACHE", CACHE_P, 1);
  }
  build_repo_parents();
  ccb_maven_result out;
  ccb_diag_list diags;
  // A: grandparent management + property through two levels.
  ccb_diag_list_init(&diags);
  const char *a[] = { "com.example:app:1.0" };
  CHECK(resolve_here(a, 1, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "standalone-2.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  // B: child property wins over the parent's.
  ccb_diag_list_init(&diags);
  const char *b[] = { "com.example:appb:1.0" };
  CHECK(resolve_here(b, 1, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "standalone-9.9.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  // C: child management wins over the parent's.
  ccb_diag_list_init(&diags);
  const char *c[] = { "com.example:appc:1.0" };
  CHECK(resolve_here(c, 1, 1, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "standalone-2.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  // D: parent cycle names the chain.
  ccb_diag_list_init(&diags);
  const char *d[] = { "com.example:cya:1.0" };
  CHECK(resolve_here(d, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3024"));
  CHECK(diag_msg_has(&diags, "com.example:cya:1.0"));
  CHECK(diag_msg_has(&diags, "com.example:cyb:1.0"));
  ccb_diag_list_free(&diags);
  // E1: prefetch with a missing parent POM fails unresolvable.
  ccb_diag_list_init(&diags);
  const char *e1[] = { "com.example:appf:1.0" };
  CHECK(resolve_here(e1, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3019"));
  ccb_diag_list_free(&diags);
  // E2: cache-only with the parent POM evicted from the cache fails
  // E3021, naming the parent.
  ccb_diag_list_init(&diags);
  const char *e2[] = { "com.example:appe:1.0" };
  CHECK(resolve_here(e2, 1, 1, &out, &diags) == CCB_OK);
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  {
    ccb_str p;
    ccb_str_init(&p);
    ccb_str_append(&p, CACHE_P);
    ccb_str_append(&p, "/com/example/pare/1.0/pare-1.0.pom");
    char *pom_path = ccb_str_steal(&p);
    CHECK(remove(pom_path) == 0);
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, pom_path);
    ccb_str_append(&s, ".ccb-sha256");
    char *side = ccb_str_steal(&s);
    remove(side);
    free(side);
    free(pom_path);
  }
  ccb_diag_list_init(&diags);
  CHECK(resolve_here(e2, 1, 0, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3021"));
  CHECK(diag_msg_has(&diags, "com.example:pare:1.0"));
  ccb_diag_list_free(&diags);
  // F: 11 parents exceed the cap; exactly 10 resolve.
  ccb_diag_list_init(&diags);
  const char *f[] = { "com.example:d0:1.0" };
  CHECK(resolve_here(f, 1, 1, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3024"));
  ccb_diag_list_free(&diags);
  ccb_diag_list_init(&diags);
  const char *g[] = { "com.example:e0:1.0" };
  CHECK(resolve_here(g, 1, 1, &out, &diags) == CCB_OK);
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
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

// ---------- exclusions + lockfile (v2 follow-ups) ----------

#define REPO_X BASE "/repo_x"
#define CACHE_X BASE "/cache_x"
#define PROJ_X BASE "/proj_x"

static char *repo_x_leaf_path(const char *group, const char *artifact,
    const char *version, const char *leaf) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, REPO_X);
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

// Write one jar artifact + POM into the exclusion/lockfile fixture
// repo. deps_xml is the raw <dependency> XML, or NULL for no deps.
static void put_x(const char *group, const char *artifact,
    const char *version, const char *deps_xml) {
  ccb_str pom;
  ccb_str_init(&pom);
  ccb_str_append(&pom, "<project><modelVersion>4.0.0</modelVersion>");
  ccb_str_append(&pom, "<groupId>");
  ccb_str_append(&pom, group);
  ccb_str_append(&pom, "</groupId><artifactId>");
  ccb_str_append(&pom, artifact);
  ccb_str_append(&pom, "</artifactId><version>");
  ccb_str_append(&pom, version);
  ccb_str_append(&pom, "</version><packaging>jar</packaging>");
  if (deps_xml != NULL) {
    ccb_str_append(&pom, "<dependencies>");
    ccb_str_append(&pom, deps_xml);
    ccb_str_append(&pom, "</dependencies>");
  }
  ccb_str_append(&pom, "</project>");
  ccb_str leaf;
  ccb_str_init(&leaf);
  ccb_str_append(&leaf, artifact);
  ccb_str_append_c(&leaf, '-');
  ccb_str_append(&leaf, version);
  ccb_str_append(&leaf, ".pom");
  char *pom_path = repo_x_leaf_path(group, artifact, version, leaf.buf);
  ccb_str_free(&leaf);
  char *dir = ccb_dirname(pom_path);
  CHECK(ccb_mkdir_p(dir) == CCB_OK);
  free(dir);
  write_text(pom_path, pom.buf);
  free(pom_path);
  ccb_str_free(&pom);
  ccb_str al;
  ccb_str_init(&al);
  ccb_str_append(&al, artifact);
  ccb_str_append_c(&al, '-');
  ccb_str_append(&al, version);
  ccb_str_append(&al, ".jar");
  char *art_path = repo_x_leaf_path(group, artifact, version, al.buf);
  ccb_str_free(&al);
  write_text(art_path, "fake-artifact-bytes");
  free(art_path);
}

static void build_repo_x(void) {
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "mid", "1.0");
    put_x("com.example", "top", "1.0", deps.buf);
    ccb_str_free(&deps);
  }
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "deep", "2.0");
    put_x("com.example", "mid", "1.0", deps.buf);
    ccb_str_free(&deps);
  }
  put_x("com.example", "deep", "1.0", NULL);
  put_x("com.example", "deep", "2.0", NULL);
  put_x("com.example", "deep", "3.0", NULL);
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "deep", "2.0");
    put_x("com.example", "a", "1.0", deps.buf);
    ccb_str_free(&deps);
  }
  {
    ccb_str deps;
    ccb_str_init(&deps);
    dep_xml(&deps, "com.example", "deep", "3.0");
    put_x("com.example", "b", "1.0", deps.buf);
    ccb_str_free(&deps);
  }
}

// Manual config with exclusions and a project root (for lockfiles).
static ccb_config *manual_cfg_x(const char **coords, size_t count,
    const char **excls, size_t excl_count, const char *root) {
  ccb_config *c = ccb_xcalloc(1, sizeof(*c));
  c->maven = ccb_xmalloc((count == 0 ? 1 : count) * sizeof(*c->maven));
  for (size_t i = 0; i < count; i++) {
    c->maven[i] = ccb_xstrdup(coords[i]);
  }
  c->maven_count = count;
  c->maven_exclude =
    ccb_xmalloc((excl_count == 0 ? 1 : excl_count) * sizeof(*c->maven_exclude));
  for (size_t i = 0; i < excl_count; i++) {
    c->maven_exclude[i] = ccb_xstrdup(excls[i]);
  }
  c->maven_exclude_count = excl_count;
  c->project_root = root != NULL ? ccb_xstrdup(root) : NULL;
  return c;
}

static ccb_status resolve_x(const char **coords, size_t count, int prefetch,
    const char **excls, size_t excl_count, const char *root,
    ccb_maven_result *out, ccb_diag_list *diags) {
  ccb_config *c = manual_cfg_x(coords, count, excls, excl_count, root);
  ccb_status rc = ccb_maven_resolve(c, prefetch, 0, 0, out, diags);
  ccb_config_free(c);
  return rc;
}

// Point the test process at the exclusion/lockfile fixture repo and
// cache, saving the previous env for the caller to restore.
static void use_repo_x(char **saved_repos, char **saved_cache) {
  const char *old_repos = getenv("CCB_MAVEN_REPOS");
  const char *old_cache = getenv("CCB_MAVEN_CACHE");
  *saved_repos = old_repos != NULL ? ccb_xstrdup(old_repos) : NULL;
  *saved_cache = old_cache != NULL ? ccb_xstrdup(old_cache) : NULL;
  ccb_str r;
  ccb_str_init(&r);
  ccb_str_append(&r, "file://");
  ccb_str_append(&r, REPO_X);
  char *repos_env = ccb_str_steal(&r);
  setenv("CCB_MAVEN_REPOS", repos_env, 1);
  free(repos_env);
  setenv("CCB_MAVEN_CACHE", CACHE_X, 1);
}

static void restore_env(char *saved_repos, char *saved_cache) {
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
}

static void test_exclusions(void) {
  char *saved_repos = NULL;
  char *saved_cache = NULL;
  use_repo_x(&saved_repos, &saved_cache);
  build_repo_x();
  ccb_maven_result out;
  ccb_diag_list diags;
  // Transitive exclusion: top -> mid -> deep, with deep excluded.
  // The build succeeds without deep; mid still resolves.
  {
    const char *coords[] = { "com.example:top:1.0" };
    const char *excls[] = { "com.example:deep" };
    ccb_diag_list_init(&diags);
    CHECK(resolve_x(coords, 1, 1, excls, 1, NULL, &out, &diags) == CCB_OK);
    CHECK(result_has(&out, "top-1.0.jar"));
    CHECK(result_has(&out, "mid-1.0.jar"));
    CHECK(!result_has(&out, "deep-2.0.jar"));
    CHECK(out.jars_count == 2);
    ccb_maven_result_free(&out);
    ccb_diag_list_free(&diags);
  }
  // Versioned exclusion changes the nearest-wins outcome: without it
  // a -> deep:2.0 wins (first-declared); with deep:2.0 excluded,
  // b -> deep:3.0 wins instead.
  {
    const char *coords[] = { "com.example:a:1.0", "com.example:b:1.0" };
    const char *excls[] = { "com.example:deep:2.0" };
    ccb_diag_list_init(&diags);
    CHECK(resolve_x(coords, 2, 1, excls, 1, NULL, &out, &diags) == CCB_OK);
    CHECK(result_has(&out, "deep-3.0.jar"));
    CHECK(!result_has(&out, "deep-2.0.jar"));
    ccb_maven_result_free(&out);
    ccb_diag_list_free(&diags);
  }
  // Unversioned exclusion removes every version of the artifact.
  {
    const char *coords[] = { "com.example:a:1.0", "com.example:b:1.0" };
    const char *excls[] = { "com.example:deep" };
    ccb_diag_list_init(&diags);
    CHECK(resolve_x(coords, 2, 1, excls, 1, NULL, &out, &diags) == CCB_OK);
    CHECK(!result_has(&out, "deep-2.0.jar"));
    CHECK(!result_has(&out, "deep-3.0.jar"));
    ccb_maven_result_free(&out);
    ccb_diag_list_free(&diags);
  }
  restore_env(saved_repos, saved_cache);
}

static void write_cfg_toml_x(const char *coord, const char *exclude) {
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
  if (exclude != NULL) {
    ccb_str_append(&s, "maven_exclude = [\"");
    ccb_str_append(&s, exclude);
    ccb_str_append(&s, "\"]\n");
  }
  char *toml = ccb_str_steal(&s);
  write_text(PROJ_CFG "/ccb.toml", toml);
  free(toml);
}

static void test_exclusion_config(void) {
  mkdir_or_fail(PROJ_CFG "/src/main/res/values");
  mkdir_or_fail(PROJ_CFG "/src/main/kotlin");
  write_text(PROJ_CFG "/src/main/AndroidManifest.xml",
    "<manifest package=\"com.example.hello\" />\n");
  // Excluding a direct dependency (unversioned or exact version) is
  // a config error naming it.
  static const char *bad[][2] = {
    { "com.example:top:1.0", "com.example:top" },
    { "com.example:top:1.0", "com.example:top:1.0" },
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    write_cfg_toml_x(bad[i][0], bad[i][1]);
    ccb_config *cfg = NULL;
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(PROJ_CFG "/ccb.toml", NULL, &cfg, &diags) != CCB_OK);
    CHECK(cfg == NULL);
    CHECK(has_code(&diags, "CCB-E1005"));
    ccb_diag_list_free(&diags);
  }
  // Excluding a different version of a direct dependency is allowed,
  // and so is a malformed-exclusion-free config with no exclusion.
  static const char *good[][2] = {
    { "com.example:top:1.0", "com.example:top:2.0" },
    { "com.example:top:1.0", "com.example:other" },
    { "com.example:top:1.0", NULL },
  };
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
    write_cfg_toml_x(good[i][0], good[i][1]);
    ccb_config *cfg = NULL;
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(PROJ_CFG "/ccb.toml", NULL, &cfg, &diags) == CCB_OK);
    CHECK(cfg != NULL && cfg->maven_count == 1);
    ccb_config_free(cfg);
    ccb_diag_list_free(&diags);
  }
  // Malformed exclusions are config errors too.
  static const char *malformed[] = {
    "com.example",
    "com.example:top:1.0:extra",
    "com.example:",
    ":top",
  };
  for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
    write_cfg_toml_x("com.example:top:1.0", malformed[i]);
    ccb_config *cfg = NULL;
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    CHECK(ccb_config_load(PROJ_CFG "/ccb.toml", NULL, &cfg, &diags) != CCB_OK);
    CHECK(cfg == NULL);
    CHECK(has_code(&diags, "CCB-E1005"));
    ccb_diag_list_free(&diags);
  }
}

static char *cache_x_file(const char *artifact, const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, CACHE_X);
  ccb_str_append(&s, "/com/example/");
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, version);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '-');
  ccb_str_append(&s, version);
  ccb_str_append(&s, ".jar");
  return ccb_str_steal(&s);
}

// Lockfile text from the real cached hashes, optionally omitting the
// deep line or appending one extra line.
static char *lock_text_real(int omit_deep, const char *extra) {
  char *deep_path = cache_x_file("deep", "2.0");
  char *mid_path = cache_x_file("mid", "1.0");
  char *top_path = cache_x_file("top", "1.0");
  char deep_hex[65];
  char mid_hex[65];
  char top_hex[65];
  CHECK(ccb_sha256_file(deep_path, deep_hex) == CCB_OK);
  CHECK(ccb_sha256_file(mid_path, mid_hex) == CCB_OK);
  CHECK(ccb_sha256_file(top_path, top_hex) == CCB_OK);
  ccb_str s;
  ccb_str_init(&s);
  if (!omit_deep) {
    ccb_str_append(&s, "com.example:deep:2.0 jar ");
    ccb_str_append(&s, deep_hex);
    ccb_str_append_c(&s, '\n');
  }
  ccb_str_append(&s, "com.example:mid:1.0 jar ");
  ccb_str_append(&s, mid_hex);
  ccb_str_append_c(&s, '\n');
  ccb_str_append(&s, "com.example:top:1.0 jar ");
  ccb_str_append(&s, top_hex);
  ccb_str_append_c(&s, '\n');
  if (extra != NULL) {
    ccb_str_append(&s, extra);
  }
  free(deep_path);
  free(mid_path);
  free(top_path);
  return ccb_str_steal(&s);
}

static void test_lockfile(void) {
  char *saved_repos = NULL;
  char *saved_cache = NULL;
  use_repo_x(&saved_repos, &saved_cache);
  build_repo_x();
  CHECK(ccb_mkdir_p(PROJ_X) == CCB_OK);
  const char *coords[] = { "com.example:top:1.0" };
  ccb_maven_result out;
  ccb_diag_list diags;
  // Prefetch ignores a stale lockfile (setup is the update path):
  // write a bogus one first, prefetch still succeeds.
  write_text(PROJ_X "/ccb.lock", "bogus line\n");
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 1, NULL, 0, PROJ_X, &out, &diags) == CCB_OK);
  // Generation: exact contents, sorted, transitive versions + hashes.
  {
    ccb_config *lc = manual_cfg_x(coords, 1, NULL, 0, PROJ_X);
    CHECK(ccb_maven_write_lockfile(lc, &out, &diags) == CCB_OK);
    ccb_config_free(lc);
  }
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  {
    char *top_path = cache_x_file("top", "1.0");
    char *mid_path = cache_x_file("mid", "1.0");
    char *deep_path = cache_x_file("deep", "2.0");
    char top_hex[65];
    char mid_hex[65];
    char deep_hex[65];
    CHECK(ccb_sha256_file(top_path, top_hex) == CCB_OK);
    CHECK(ccb_sha256_file(mid_path, mid_hex) == CCB_OK);
    CHECK(ccb_sha256_file(deep_path, deep_hex) == CCB_OK);
    ccb_str want;
    ccb_str_init(&want);
    ccb_str_append(&want,
      "# ccb.lock: resolved Maven tree, generated by `ccb setup`.\n"
      "# Commit this file. `ccb check` and `ccb build` enforce it: any\n"
      "# deviation from these versions or hashes fails with CCB-E3023.\n"
      "# ccb-lock-version: 1\n"
      "com.example:deep:2.0 jar ");
    ccb_str_append(&want, deep_hex);
    ccb_str_append(&want, "\ncom.example:mid:1.0 jar ");
    ccb_str_append(&want, mid_hex);
    ccb_str_append(&want, "\ncom.example:top:1.0 jar ");
    ccb_str_append(&want, top_hex);
    ccb_str_append_c(&want, '\n');
    char *got = NULL;
    size_t got_len = 0;
    CHECK(ccb_read_file(PROJ_X "/ccb.lock", &got, &got_len) == CCB_OK);
    CHECK(got_len == strlen(want.buf));
    CHECK(memcmp(got, want.buf, got_len) == 0);
    free(got);
    ccb_str_free(&want);
    free(top_path);
    free(mid_path);
    free(deep_path);
  }
  // Locked build: cache-only resolve with the lockfile succeeds and
  // uses exactly the locked versions.
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 0, NULL, 0, PROJ_X, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "deep-2.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  // Mismatch cases, each failing with CCB-E3023 naming the coordinate.
  // Version drift: the lockfile pins deep:9.9, resolution finds 2.0.
  write_text(PROJ_X "/ccb.lock",
    "com.example:deep:9.9 jar 0000000000000000000000000000000000000000000000000000000000000000\n"
    "com.example:mid:1.0 jar 0000000000000000000000000000000000000000000000000000000000000000\n"
    "com.example:top:1.0 jar 0000000000000000000000000000000000000000000000000000000000000000\n");
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 0, NULL, 0, PROJ_X, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3023"));
  ccb_diag_list_free(&diags);
  // Hash drift: right versions, wrong hash for deep.
  {
    char *lock_text = lock_text_real(0, NULL);
    // Zero deep's hash in place: the line is fixed-width, so find the
    // first line and overwrite its 64-char hash field.
    char *sp = strchr(lock_text, ' ');
    char *sp2 = sp != NULL ? strchr(sp + 1, ' ') : NULL;
    CHECK(sp2 != NULL);
    memset(sp2 + 1, '0', 64);
    write_text(PROJ_X "/ccb.lock", lock_text);
    free(lock_text);
  }
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 0, NULL, 0, PROJ_X, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3023"));
  ccb_diag_list_free(&diags);
  // Missing entry: deep resolved but absent from the lockfile (all
  // other lines match, so only the missing line can fail).
  {
    char *lock_text = lock_text_real(1, NULL);
    write_text(PROJ_X "/ccb.lock", lock_text);
    free(lock_text);
  }
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 0, NULL, 0, PROJ_X, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3023"));
  ccb_diag_list_free(&diags);
  // Stale entry: locked but no longer resolved (all resolved entries
  // match, so only the stale line can fail).
  {
    char *lock_text = lock_text_real(0,
      "com.example:gone:1.0 jar 0000000000000000000000000000000000000000000000000000000000000000\n");
    write_text(PROJ_X "/ccb.lock", lock_text);
    free(lock_text);
  }
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 0, NULL, 0, PROJ_X, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3023"));
  ccb_diag_list_free(&diags);
  // Malformed lockfile.
  write_text(PROJ_X "/ccb.lock", "this is not a lockfile line\n");
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 0, NULL, 0, PROJ_X, &out, &diags) != CCB_OK);
  CHECK(has_code(&diags, "CCB-E3023"));
  ccb_diag_list_free(&diags);
  // No lockfile: pre-lockfile behavior, cache-only resolve succeeds.
  CHECK(remove(PROJ_X "/ccb.lock") == 0);
  ccb_diag_list_init(&diags);
  CHECK(resolve_x(coords, 1, 0, NULL, 0, PROJ_X, &out, &diags) == CCB_OK);
  CHECK(result_has(&out, "deep-2.0.jar"));
  ccb_maven_result_free(&out);
  ccb_diag_list_free(&diags);
  restore_env(saved_repos, saved_cache);
}

// ---------- end-to-end build ----------

#define REPO_E2E BASE "/repo_e2e"
#define CACHE_E2E BASE "/cache_e2e"
#define PROJ_E2E BASE "/proj_e2e"
#define APK_E2E PROJ_E2E "/build/outputs/debug/app.apk"
#define CACHE_E2E_P BASE "/cache_e2e_parents"
#define PROJ_E2E_P BASE "/proj_e2e_parents"
#define APK_E2E_P PROJ_E2E_P "/build/outputs/debug/app.apk"

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

static void build_e2e_repo_parents(void) {
  // Parent POM: manages lib at 1.0 for its children.
  ccb_str pd;
  ccb_str_init(&pd);
  ccb_str_append(&pd, REPO_E2E);
  ccb_str_append(&pd, "/com/example/par/1.0");
  char *pardir = ccb_str_steal(&pd);
  CHECK(ccb_mkdir_p(pardir) == CCB_OK);
  ccb_str pp;
  ccb_str_init(&pp);
  ccb_str_append(&pp, pardir);
  ccb_str_append(&pp, "/par-1.0.pom");
  char *par_pom = ccb_str_steal(&pp);
  write_text(par_pom,
    "<project><modelVersion>4.0.0</modelVersion>"
    "<groupId>com.example</groupId><artifactId>par</artifactId>"
    "<version>1.0</version><packaging>pom</packaging>"
    "<dependencyManagement><dependencies><dependency>"
    "<groupId>com.example</groupId><artifactId>lib</artifactId>"
    "<version>1.0</version></dependency></dependencies>"
    "</dependencyManagement></project>");
  free(par_pom);
  free(pardir);
  // Res-only AAR whose dependency version comes from the parent.
  // Its groupId is inherited from the parent block, too.
  ccb_str d;
  ccb_str_init(&d);
  ccb_str_append(&d, REPO_E2E);
  ccb_str_append(&d, "/com/example/plib/1.0");
  char *libdir = ccb_str_steal(&d);
  CHECK(ccb_mkdir_p(libdir) == CCB_OK);
  ccb_str aar;
  ccb_str_init(&aar);
  ccb_str_append(&aar, libdir);
  ccb_str_append(&aar, "/plib-1.0.aar");
  char *aar_path = ccb_str_steal(&aar);
  ccb_zip_writer w;
  CHECK(ccb_zip_writer_init(&w, aar_path) == CCB_OK);
  const char *plib_manifest =
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "    package=\"com.example.plib\">\n"
    "    <application />\n"
    "</manifest>\n";
  const char *plib_strings =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"plib_hello\">Hello from plib!</string>\n"
    "</resources>\n";
  CHECK(ccb_zip_writer_add_data(&w, "AndroidManifest.xml", plib_manifest,
    strlen(plib_manifest)) == CCB_OK);
  CHECK(ccb_zip_writer_add_data(&w, "res/values/strings.xml", plib_strings,
    strlen(plib_strings)) == CCB_OK);
  CHECK(ccb_zip_writer_finish(&w) == CCB_OK);
  ccb_str pom;
  ccb_str_init(&pom);
  ccb_str_append(&pom, libdir);
  ccb_str_append(&pom, "/plib-1.0.pom");
  char *pom_path = ccb_str_steal(&pom);
  write_text(pom_path,
    "<project><modelVersion>4.0.0</modelVersion>"
    "<parent><groupId>com.example</groupId><artifactId>par</artifactId>"
    "<version>1.0</version></parent>"
    "<artifactId>plib</artifactId>"
    "<version>1.0</version><packaging>aar</packaging>"
    "<dependencies><dependency><groupId>com.example</groupId>"
    "<artifactId>lib</artifactId></dependency></dependencies></project>");
  free(pom_path);
  free(aar_path);
  free(libdir);
}

static void write_e2e_project_parents(void) {
  mkdir_or_fail(PROJ_E2E_P "/src/main/kotlin/com/example/hello");
  mkdir_or_fail(PROJ_E2E_P "/src/main/res/values");
  mkdir_or_fail(PROJ_E2E_P "/src/main/res/layout");
  write_text(PROJ_E2E_P "/ccb.toml",
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
    "maven = [\"com.example:plib:1.0\"]\n");
  write_text(PROJ_E2E_P "/src/main/AndroidManifest.xml",
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
  write_text(PROJ_E2E_P "/src/main/kotlin/com/example/hello/MainActivity.kt",
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
  write_text(PROJ_E2E_P "/src/main/res/values/strings.xml",
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <string name=\"app_name\">hello</string>\n"
    "</resources>\n");
  write_text(PROJ_E2E_P "/src/main/res/values/themes.xml",
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<resources>\n"
    "  <style name=\"AppTheme\" parent=\"android:Theme.Material\" />\n"
    "</resources>\n");
  write_text(PROJ_E2E_P "/src/main/res/layout/activity_main.xml",
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\">\n"
    "</LinearLayout>\n");
}

static void test_e2e_parents(int argc, char **argv) {
  char *bin = find_binary(argc, argv);
  char *sdk_home = tool_home("ANDROID_HOME", "android-sdk");
  char *java_home = tool_home("JAVA_HOME", "jdk17");
  char *kotlin_home = tool_home("KOTLIN_HOME", "kotlin-2.4.21");
  char *aapt2 = sdk_home == NULL ? NULL
    : ccb_path_join3(sdk_home, "build-tools/35.0.0", "aapt2");
  char *jarsigner = java_home == NULL ? NULL : ccb_path_join(java_home, "bin/jarsigner");
  if (bin == NULL || aapt2 == NULL || access(aapt2, X_OK) != 0 ||
      jarsigner == NULL || access(jarsigner, X_OK) != 0) {
    fprintf(stderr, "note: no runnable SDK toolchain, skipping parents e2e build checks\n");
    free(bin);
    free(sdk_home);
    free(java_home);
    free(kotlin_home);
    free(aapt2);
    free(jarsigner);
    return;
  }
  build_e2e_repo();
  build_e2e_repo_parents();
  write_e2e_project_parents();
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
    setenv("CCB_MAVEN_CACHE", CACHE_E2E_P, 1);
  }
  {
    const char *coords[] = { "com.example:plib:1.0" };
    ccb_maven_result out;
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    CHECK(resolve_here(coords, 1, 1, &out, &diags) == CCB_OK);
    CHECK(result_has(&out, "plib-1.0.aar"));
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
  ccb_str_append(&s, CACHE_E2E_P);
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
    CHECK(ccb_proc_run(args, PROJ_E2E_P, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(strstr(r.out, "maven:") != NULL);
    ccb_proc_result_free(&r);
  }
  {
    char *args[] = { bin, "build", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(args, PROJ_E2E_P, env_extra, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK(ccb_path_exists(APK_E2E_P));
    ccb_proc_result_free(&r);
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
  test_parents();
  test_config_validation();
  test_exclusions();
  test_exclusion_config();
  test_lockfile();
  test_e2e(argc, argv);
  test_e2e_parents(argc, argv);
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
