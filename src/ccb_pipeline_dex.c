// ccb_pipeline_dex: R8 keep rules + dex step + dexcheck stage.
// Split from ccb_pipeline.c.

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_proc.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_cache.h"
#include "ccb_manifest.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_resns.h"
#include "ccb_zip.h"
#include "ccb_bundle.h"
#include "ccb_dexcheck.h"
#include "ccb_daemon.h"
#include "ccb_pipeline.h"
#include <sys/stat.h>
#include <unistd.h>
#include "ccb_pipeline_internal.h"

// ---------- R8 keep rules (opt-apk) ----------
//
// A minify build runs R8, which tree-shakes everything not reachable
// from a kept entry point -- including manifest components and layout
// view classes, which no code references. With no keeps at all R8
// shrinks the whole program away and writes no dex (verified on the
// Showcase: the dex dir held only resource files). CCB therefore
// generates the keep rules for those entry points, so the project's
// own proguard_rules file only needs the keeps R8 cannot infer at
// all: reflection-only references via Class.forName strings. The
// generated file lives at build/intermediates/r8-keep.pro and is
// passed to R8 ahead of the project's rules. Its text is part of the
// dex fingerprint, so a manifest or layout change re-runs dex.
//
// Collected, in order: the app manifest's entry points, every AAR
// manifest's entry points (declared AARs from their zips, library
// modules from their manifest files -- the same union the merged
// manifest holds, so the rules are identical in real, explain, and
// dry-run modes), and the view classes referenced from layout XML
// (app + module res dirs, declared AAR zips). The stdlib dex cache
// stays bypassed on this path, as before: R8 takes the stdlib as a
// program-input JAR, not dex, because only R8 can see which stdlib
// classes the kept code actually reaches.

static void ccb_r8_push(char ***arr, size_t *count, size_t *cap, const char *s) {
  if (*count == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    *arr = ccb_xrealloc(*arr, ncap * sizeof(**arr));
    *cap = ncap;
  }
  (*arr)[(*count)++] = ccb_xstrdup(s);
}

static int ccb_r8_is_name_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
    (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '$';
}

// True when s (len bytes) looks like a fully-qualified class name:
// a letter or underscore first, then name chars, with at least one dot.
static int ccb_r8_is_class_name(const char *s, size_t len) {
  if (len < 3) {
    return 0;
  }
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) {
    return 0;
  }
  int dot = 0;
  for (size_t i = 1; i < len; i++) {
    if (!ccb_r8_is_name_char(s[i])) {
      return 0;
    }
    if (s[i] == '.') {
      dot = 1;
    }
  }
  return dot;
}

// Index just past the markup starting at i ("<!--...-->", "<?...?>",
// "<!...>", "</...>"), or len when unterminated.
static size_t ccb_r8_skip_markup(const char *text, size_t len, size_t i) {
  if (i + 3 < len && strncmp(text + i, "<!--", 4) == 0) {
    size_t j = i + 4;
    while (j + 2 < len && strncmp(text + j, "-->", 3) != 0) {
      j++;
    }
    return j + 3 <= len ? j + 3 : len;
  }
  if (i + 1 < len && text[i + 1] == '?') {
    size_t j = i + 2;
    while (j + 1 < len && strncmp(text + j, "?>", 2) != 0) {
      j++;
    }
    return j + 2 <= len ? j + 2 : len;
  }
  size_t j = i + 1;
  while (j < len && text[j] != '>') {
    j++;
  }
  return j < len ? j + 1 : len;
}

// Class-valued layout attributes, scanned with their opening quote:
// <view class="...">, <fragment android:name="...">, and
// app:layout_behavior="..." (CoordinatorLayout behaviors).
// tools:context is design-time only and is deliberately not scanned.
static const char *ccb_r8_layout_attrs[] = {
  "class=\"", "class='",
  "android:name=\"", "android:name='",
  "layout_behavior=\"", "layout_behavior='",
};

// Attribute match at i: returns the characters consumed (through the
// end of the value) when text[i] starts a class-valued attribute,
// else 0.
static size_t ccb_r8_scan_attr(const char *text, size_t len, size_t i,
    char ***out, size_t *n, size_t *cap) {
  for (size_t a = 0; a < CCB_ARRAY_LEN(ccb_r8_layout_attrs); a++) {
    const char *attr = ccb_r8_layout_attrs[a];
    size_t alen = strlen(attr);
    if (i + alen > len || strncmp(text + i, attr, alen) != 0) {
      continue;
    }
    size_t start = i + alen;
    size_t j = start;
    while (j < len && text[j] != '"' && text[j] != '\'') {
      j++;
    }
    if (ccb_r8_is_class_name(text + start, j - start)) {
      char *name = ccb_xstrndup(text + start, j - start);
      ccb_r8_push(out, n, cap, name);
      free(name);
    }
    return j - i;
  }
  return 0;
}

// Scan one layout XML text for class references: element names with a
// dot (custom views) and the class-valued attributes above. Comments
// and processing instructions are skipped, so a dotted name inside a
// comment never becomes a keep.
static void ccb_r8_scan_layout_text(const char *text, char ***out, size_t *n, size_t *cap) {
  size_t len = strlen(text);
  size_t i = 0;
  while (i < len) {
    if (text[i] == '<' && i + 1 < len &&
        (text[i + 1] == '!' || text[i + 1] == '?' || text[i + 1] == '/')) {
      i = ccb_r8_skip_markup(text, len, i);
      continue;
    }
    if (text[i] == '<') {
      size_t start = i + 1;
      size_t j = start;
      while (j < len && (ccb_r8_is_name_char(text[j]) || text[j] == '-' || text[j] == ':')) {
        j++;
      }
      if (j > start && ccb_r8_is_class_name(text + start, j - start)) {
        char *name = ccb_xstrndup(text + start, j - start);
        ccb_r8_push(out, n, cap, name);
        free(name);
      }
      i = j;
      continue;
    }
    size_t adv = ccb_r8_scan_attr(text, len, i, out, n, cap);
    i += adv == 0 ? 1 : adv;
  }
}

// True when path sits directly under a layout dir ("layout" or
// "layout-...").
static int ccb_r8_is_layout_file(const char *path) {
  char *dir = ccb_dirname(path);
  char *base = ccb_basename(dir);
  int ok = strncmp(base, "layout", 6) == 0 && (base[6] == '\0' || base[6] == '-');
  free(dir);
  free(base);
  return ok;
}

// Scan the layout XML files under one res dir. A missing res dir
// contributes nothing. Returns 0 on success, -1 on a read failure.
static int ccb_r8_scan_layout_dir(const char *res_dir, char ***out, size_t *n, size_t *cap) {
  if (res_dir == NULL) {
    return 0;
  }
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(res_dir, ".xml", &files, &nfiles) != CCB_OK) {
    return -1;
  }
  int rc = 0;
  for (size_t i = 0; i < nfiles; i++) {
    if (!ccb_r8_is_layout_file(files[i])) {
      continue;
    }
    char *text = NULL;
    size_t tlen = 0;
    if (ccb_read_file(files[i], &text, &tlen) != CCB_OK) {
      rc = -1;
      break;
    }
    ccb_r8_scan_layout_text(text, out, n, cap);
    free(text);
  }
  ccb_free_str_array(files, nfiles);
  return rc;
}

// Scan the layout XML entries of one AAR zip (res/layout*/*.xml).
// An AAR with no layouts contributes nothing. Returns 0 on success,
// -1 when the zip cannot be opened or an entry cannot be read.
static int ccb_r8_scan_aar_layouts(const char *aar_path, char ***out, size_t *n, size_t *cap) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(aar_path, &a) != CCB_OK) {
    return -1;
  }
  int rc = 0;
  for (size_t i = 0; i < a->count && rc == 0; i++) {
    const ccb_zip_entry *e = &a->entries[i];
    if (strncmp(e->name, "res/layout", 10) != 0) {
      continue;
    }
    if (e->name[10] != '/' && e->name[10] != '-') {
      continue;
    }
    size_t nl = strlen(e->name);
    if (nl < 4 || strcmp(e->name + nl - 4, ".xml") != 0) {
      continue;
    }
    void *data = NULL;
    size_t dlen = 0;
    if (ccb_zip_read_entry(a, e, &data, &dlen) != CCB_OK) {
      rc = -1;
      break;
    }
    char *text = ccb_xmalloc(dlen + 1);
    memcpy(text, data, dlen);
    text[dlen] = '\0';
    free(data);
    ccb_r8_scan_layout_text(text, out, n, cap);
    free(text);
  }
  ccb_zip_close(a);
  return rc;
}

// Append one manifest document's entry points. manifest_path is a
// file on disk; fallback_pkg resolves relative names when the
// manifest declares no package (the app package, for AARs).
// Returns 0 on success, -1 on a read or parse failure.
static int ccb_r8_add_manifest_file(const char *manifest_path,
    const char *fallback_pkg, char ***out, size_t *n, size_t *cap) {
  char *text = NULL;
  size_t tlen = 0;
  if (ccb_read_file(manifest_path, &text, &tlen) != CCB_OK) {
    return -1;
  }
  char **names = NULL;
  size_t nnames = 0;
  char *err = NULL;
  int rc = ccb_manifest_entry_points(text, fallback_pkg, &names, &nnames, &err);
  free(err);
  free(text);
  if (rc != 0) {
    return -1;
  }
  for (size_t i = 0; i < nnames; i++) {
    ccb_r8_push(out, n, cap, names[i]);
  }
  ccb_free_str_array(names, nnames);
  return 0;
}

// Append one AAR manifest's entry points from the AAR zip. An AAR
// with no manifest contributes nothing. Returns 0 on success, -1
// when the zip cannot be read or the manifest does not parse.
static int ccb_r8_add_aar_manifest(const char *aar_path,
    const char *fallback_pkg, char ***out, size_t *n, size_t *cap) {
  int failed = 0;
  char *text = ccb_aar_manifest_text(aar_path, &failed);
  if (failed) {
    return -1;
  }
  if (text == NULL) {
    return 0;
  }
  char **names = NULL;
  size_t nnames = 0;
  char *err = NULL;
  int rc = ccb_manifest_entry_points(text, fallback_pkg, &names, &nnames, &err);
  free(err);
  free(text);
  if (rc != 0) {
    return -1;
  }
  for (size_t i = 0; i < nnames; i++) {
    ccb_r8_push(out, n, cap, names[i]);
  }
  ccb_free_str_array(names, nnames);
  return 0;
}

// Collect every class name the keep rules must cover: manifest entry
// points (app + AARs + modules) and layout view classes (app + AARs
// + modules). Returns 0 on success, -1 when an input cannot be read
// or a manifest does not parse.
static int ccb_r8_collect_names(ccb_pipe *p, char ***out, size_t *n, size_t *cap) {
  if (ccb_r8_add_manifest_file(p->cfg->manifest, NULL, out, n, cap) != 0) {
    return -1;
  }
  if (ccb_r8_scan_layout_dir(p->cfg->res, out, n, cap) != 0) {
    return -1;
  }
  for (size_t i = 0; i < p->cfg->aars_count; i++) {
    if (ccb_r8_add_aar_manifest(p->cfg->aars[i], p->cfg->package, out, n, cap) != 0) {
      return -1;
    }
    if (ccb_r8_scan_aar_layouts(p->cfg->aars[i], out, n, cap) != 0) {
      return -1;
    }
  }
  for (size_t i = 0; i < p->cfg->modules_count; i++) {
    ccb_module *m = &p->cfg->modules[i];
    if (ccb_r8_add_manifest_file(m->manifest, p->cfg->package, out, n, cap) != 0) {
      return -1;
    }
    if (ccb_r8_scan_layout_dir(m->res, out, n, cap) != 0) {
      return -1;
    }
  }
  return 0;
}

static int ccb_r8_cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// The generated keep rules text, or NULL when an input cannot be
// read or a manifest does not parse. The caller frees the result.
static char *ccb_r8_keep_rules(ccb_pipe *p) {
  char **names = NULL;
  size_t n = 0;
  size_t cap = 0;
  if (ccb_r8_collect_names(p, &names, &n, &cap) != 0) {
    ccb_free_str_array(names, n);
    return NULL;
  }
  if (n > 1) {
    qsort(names, n, sizeof(*names), ccb_r8_cmp_str);
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    if (kept > 0 && strcmp(names[i], names[kept - 1]) == 0) {
      free(names[i]);
      continue;
    }
    names[kept++] = names[i];
  }
  n = kept;
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s,
    "# Generated by CCB for release + minify: entry points R8 cannot\n"
    "# see from code (manifest components, layout view classes). The\n"
    "# app's own classes are kept whole by the package rule below;\n"
    "# the project's own proguard_rules file is passed to R8 as well\n"
    "# -- put reflection-only keeps (Class.forName strings) there, CCB\n"
    "# cannot infer them.\n");
  ccb_str_append(&s, "-keep class ");
  ccb_str_append(&s, p->cfg->package);
  ccb_str_append(&s, ".** { *; }\n");
  size_t plen = strlen(p->cfg->package);
  for (size_t i = 0; i < n; i++) {
    if (strncmp(names[i], p->cfg->package, plen) == 0 && names[i][plen] == '.') {
      continue;
    }
    ccb_str_append(&s, "-keep class ");
    ccb_str_append(&s, names[i]);
    ccb_str_append(&s, " { *; }\n");
  }
  ccb_free_str_array(names, n);
  return ccb_str_steal(&s);
}

static size_t ccb_dex_count_files(const char *dir, const char *ext) {
  if (dir == NULL) {
    return 0;
  }
  char **files = NULL;
  size_t n = 0;
  if (ccb_collect_files(dir, ext, &files, &n) != CCB_OK) {
    return 0;
  }
  ccb_free_str_array(files, n);
  return n;
}

// True when the project has code sources: Kotlin/Java sources,
// generated R.java, or vendored java_extra roots. A build with
// sources must produce classes, so "no classes" at dex time is an
// error there — not a resources-only skip.
static int ccb_dex_has_sources(ccb_pipe *p) {
  if (ccb_dex_count_files(p->cfg->kotlin_dir, ".kt") > 0) {
    return 1;
  }
  if (ccb_dex_count_files(p->cfg->java_dir, ".java") > 0) {
    return 1;
  }
  if (ccb_dex_count_files(p->gen_dir, ".java") > 0) {
    return 1;
  }
  for (size_t d = 0; d < p->cfg->java_extra_dirs_count; d++) {
    if (ccb_dex_count_files(p->cfg->java_extra_dirs[d], ".java") > 0) {
      return 1;
    }
  }
  return 0;
}

// True when the dex dir holds at least one whole .dex file (dex
// magic + header size). A truncated .dex must re-dex, never skip.
static int ccb_dex_dir_valid(ccb_pipe *p) {
  if (!ccb_path_is_dir(p->dex_dir)) {
    return 0;
  }
  char **dexes = NULL;
  size_t ndex = 0;
  if (ccb_collect_files(p->dex_dir, ".dex", &dexes, &ndex) != CCB_OK) {
    return 0;
  }
  int ok = ndex > 0;
  for (size_t i = 0; i < ndex && ok; i++) {
    if (!ccb_dex_file_valid(dexes[i])) {
      ok = 0;
    }
  }
  ccb_free_str_array(dexes, ndex);
  return ok;
}

ccb_status ccb_step_dex(ccb_pipe *p) {
  CCB_AUTO_FREE char *stdlib = ccb_dex_stdlib(p);
  int use_r8 = strcmp(p->cfg->variant, "release") == 0 && p->cfg->minify;
  if (use_r8 && p->sdk->r8_jar == NULL) {
    ccb_pipe_diag(p, "CCB-E3014", "r8",
      "release minification needs r8: set [sdk] r8_jar to an r8 jar "
      "(com.android.tools:r8 from Google's Maven repository), or run "
      "`ccb setup` in a minify project to install it");
    return CCB_ERR_BUILD;
  }
  // Generated keep rules for R8 (opt-apk): manifest components,
  // layout view classes, and the app's own package. Computed before
  // the fingerprint so the rules text is part of it.
  CCB_AUTO_FREE char *keep_rules = NULL;
  if (use_r8) {
    keep_rules = ccb_r8_keep_rules(p);
    if (keep_rules == NULL) {
      ccb_pipe_diag(p, "CCB-E3017", "r8",
        "cannot generate R8 keep rules from the manifest and layouts");
      return CCB_ERR_BUILD;
    }
  }
  char **class_files = NULL;
  size_t nclass = 0;
  if (ccb_path_is_dir(p->classes_dir)) {
    ccb_collect_files(p->classes_dir, ".class", &class_files, &nclass);
  }
  // A project with code sources must reach dex with classes. When
  // it does not, compile-code produced nothing (or its output was
  // lost) — dexing the jars alone would ship an APK without the
  // app's classes, so fail loudly instead of skipping silently.
  // Planning modes are exempt: on a fresh tree the classes do not
  // exist yet, and --explain reports the run via code_will_run.
  if (!p->opts->explain && !p->opts->dry_run && nclass == 0 &&
      ccb_dex_has_sources(p)) {
    ccb_pipe_diag(p, "CCB-E3006", "d8",
      "no compiled classes found, but the project has code sources: "
      "the compile-code output is missing or empty — delete build/ "
      "and rebuild if this persists");
    ccb_free_str_array(class_files, nclass);
    return CCB_ERR_BUILD;
  }
  if (nclass == 0 && p->cfg->jars_count == 0 && p->aar_class_jars_count == 0) {
    if (p->opts->explain) {
      if (ccb_dex_has_sources(p) && p->code_will_run) {
        printf("dex: run (classes will change)\n");
        p->dex_will_run = 1;
      } else {
        printf("dex: skip (no classes or jars)\n");
        p->dex_will_run = 0;
        p->steps_skipped++;
      }
    } else {
      ccb_log_info(&p->log, "no classes or jars, skipping dex");
      p->dex_will_run = 0;
      p->steps_skipped++;
    }
    ccb_log_cache(&p->log, "dex", 1, "no classes or jars");
    ccb_free_str_array(class_files, nclass);
    return CCB_OK;
  }
  ccb_free_str_array(class_files, nclass);
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add(&b, "build_tools", p->sdk->build_tools_version);
  ccb_fp_addl(&b, "min_sdk", p->cfg->min_sdk);
  ccb_fp_add(&b, "classes", p->classes_fp == NULL ? "" : p->classes_fp);
  int failed = 0;
  if (use_r8) {
    ccb_fp_add(&b, "tool", "r8");
    if (ccb_fp_add_file(p, &b, p->sdk->r8_jar) != 0) {
      failed = 1;
    }
    if (!failed && ccb_fp_add_file(p, &b, p->cfg->proguard_rules) != 0) {
      failed = 1;
    }
    if (!failed) {
      ccb_fp_add(&b, "r8-keep", keep_rules);
    }
  }
  for (size_t i = 0; i < p->cfg->jars_count && !failed; i++) {
    if (ccb_fp_add_file(p, &b, p->cfg->jars[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < p->aar_class_jars_count && !failed; i++) {
    // In explain/dry-run mode on a fresh tree, a library module's
    // classes.jar does not exist yet (modules never execute in
    // planning modes); skipping it here keeps planning working, and
    // the missing jar still fails the real build at d8 time.
    if ((p->opts->explain || p->opts->dry_run) &&
        !ccb_path_exists(p->aar_class_jars[i])) {
      continue;
    }
    if (ccb_fp_add_file(p, &b, p->aar_class_jars[i]) != 0) {
      failed = 1;
    }
  }
  if (!failed && stdlib != NULL && ccb_fp_add_file(p, &b, stdlib) != 0) {
    failed = 1;
  }
  char *fp = NULL;
  const char *old = NULL;
  if (!failed) {
    fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
    old = ccb_cache_step_fp(p->cache, "dex");
  }
  ccb_fp_free(&b);
  if (failed) {
    ccb_pipe_diag(p, "CCB-E3006", "d8", "cannot read dex inputs");
    return CCB_ERR_BUILD;
  }
  if (p->opts->explain) {
    if (p->code_will_run) {
      printf("dex: run (classes will change)\n");
      p->dex_will_run = 1;
    } else if (old != NULL && strcmp(old, fp) == 0 && ccb_dex_dir_valid(p)) {
      printf("dex: skip (inputs unchanged)\n");
      p->dex_will_run = 0;
    } else if (old != NULL && strcmp(old, fp) == 0) {
      printf("dex: run (dex outputs missing or invalid)\n");
      p->dex_will_run = 1;
    } else {
      printf("dex: run (dex inputs changed)\n");
      p->dex_will_run = 1;
    }
    free(fp);
    return CCB_OK;
  }
  // A fingerprint match alone never skips: every dex output must
  // still be present and whole, or d8 re-runs.
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_dex_dir_valid(p)) {
    free(fp);
    p->dex_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "dex", 1, "dex inputs unchanged");
    return CCB_OK;
  }
  p->dex_will_run = 1;
  if (!p->opts->dry_run) {
    ccb_remove_recursive(p->dex_dir);
    if (ccb_mkdir_p(p->dex_dir) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "d8", "cannot create dex directory");
      free(fp);
      return CCB_ERR_BUILD;
    }
  }
  // The generated keep rules file (R8 only): written in real mode,
  // named in the command in dry-run mode like every other
  // intermediate the command references.
  CCB_AUTO_FREE char *keep_path = NULL;
  if (use_r8) {
    keep_path = ccb_path_join(p->intermediates, "r8-keep.pro");
    if (!p->opts->dry_run &&
        ccb_write_file(keep_path, keep_rules, strlen(keep_rules)) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "r8", "cannot write generated R8 keep rules");
      free(fp);
      return CCB_ERR_BUILD;
    }
  }
  // The stdlib input for d8: the cached dex when one is valid, else
  // the jar. R8 takes the jar (program input), never a dex.
  CCB_AUTO_FREE char *stdlib_dex = NULL;
  const char *stdlib_input = stdlib;
  if (!use_r8 && stdlib != NULL) {
    stdlib_dex = ccb_stdlib_dex_input(p, stdlib);
    if (stdlib_dex != NULL) {
      stdlib_input = stdlib_dex;
    }
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  if (use_r8) {
    // R8 shrinks + obfuscates + dexes in one pass. The generated
    // keep rules (above) keep the manifest components, layout view
    // classes, and the app's own classes; the project's proguard
    // rules only add the keeps CCB cannot infer (reflection).
    // The r8 jar's manifest names a Main-Class it does not contain, so
    // java -jar fails; invoke the compiler class directly on the classpath.
    ccb_argv_push(&av, p->sdk->java);
    ccb_argv_push(&av, "-cp");
    ccb_argv_push(&av, p->sdk->r8_jar);
    ccb_argv_push(&av, "com.android.tools.r8.R8");
    ccb_argv_push(&av, "--release");
    ccb_argv_push(&av, "--min-api");
    ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
    ccb_argv_push(&av, "--lib");
    ccb_argv_push(&av, p->sdk->android_jar);
    // R8 fails on classes referenced from program code (unlike d8, which
    // tolerates them), so the annotations jar kotlinc output references
    // must be a library input too. The stdlib itself is a program
    // input now (added below, like the d8 path), never a --lib: the
    // same jar as both would be a duplicate. When it is not a
    // program input anywhere (no Kotlin sources and no [deps]/Maven
    // stdlib jar), it stays a --lib so R8 can still resolve Kotlin
    // references from library code.
    if (stdlib == NULL && !ccb_dex_has_stdlib(p)) {
      char *sl = ccb_path_join3(p->sdk->kotlin_home, "lib", "kotlin-stdlib.jar");
      if (sl != NULL && access(sl, R_OK) == 0) {
        ccb_argv_push(&av, "--lib");
        ccb_argv_push(&av, sl);
      }
      free(sl);
    }
    {
      char *annot = ccb_path_join3(p->sdk->kotlin_home, "lib", "annotations-13.0.jar");
      if (annot != NULL && access(annot, R_OK) == 0) {
        ccb_argv_push(&av, "--lib");
        ccb_argv_push(&av, annot);
      }
      free(annot);
    }
    ccb_argv_push(&av, "--pg-conf");
    ccb_argv_push(&av, keep_path);
    ccb_argv_push(&av, "--pg-conf");
    ccb_argv_push(&av, p->cfg->proguard_rules);
    ccb_argv_push(&av, "--output");
    ccb_argv_push(&av, p->dex_dir);
  } else {
    ccb_argv_push(&av, p->sdk->d8);
    // Sized heap (see ccb_d8_heap_mb): the -J flag must lead, so
    // the wrapper takes it as the java heap instead of its 2G
    // default, and it also beats JAVA_TOOL_OPTIONS=-Xmx1g.
    ccb_argv_pushf(&av, "-JXmx%ldm",
      ccb_d8_heap_mb(ccb_dex_input_bytes(p, stdlib_input)));
    ccb_argv_push(&av, "--min-api");
    ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
    ccb_argv_push(&av, "--lib");
    ccb_argv_push(&av, p->sdk->android_jar);
    ccb_argv_push(&av, "--output");
    ccb_argv_push(&av, p->dex_dir);
  }
  // Pass .class files individually, not the directory: kotlinc also
  // writes META-INF/*.kotlin_module into the output dir, and d8 rejects
  // non-class files found in a directory input.
  {
    char **cfiles = NULL;
    size_t ncfiles = 0;
    ccb_collect_files(p->classes_dir, ".class", &cfiles, &ncfiles);
    for (size_t i = 0; i < ncfiles; i++) {
      ccb_argv_push(&av, cfiles[i]);
    }
    ccb_free_str_array(cfiles, ncfiles);
  }
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    ccb_argv_push(&av, p->cfg->jars[i]);
  }
  for (size_t i = 0; i < p->aar_class_jars_count; i++) {
    ccb_argv_push(&av, p->aar_class_jars[i]);
  }
  if (stdlib_input != NULL) {
    ccb_argv_push(&av, stdlib_input);
  }
  char **env = ccb_jvm_env(p, ar);
  ccb_status rc;
  if (use_r8) {
    rc = ccb_run_tool(p, "r8", "CCB-E3017", av.v, env,
      "check the proguard rules and that all entry points are kept");
  } else {
    rc = ccb_run_tool(p, "d8", "CCB-E3006", av.v, env,
      "check that all classes are compatible with the min SDK");
  }
  free(av.v);
  if (rc != CCB_OK) {
    free(fp);
    return CCB_ERR_BUILD;
  }
  ccb_cache_set_step(p->cache, "dex", fp);
  free(fp);
  p->cache_dirty = 1;
  p->steps_run++;
  return CCB_OK;
}

// ---------- dexcheck ----------
//
// Post-build check (change-dexcheck): every class the dex
// references must be defined in the dex, or the build stops here,
// before packaging. This is what the Intrinsics and RecyclerView
// R$attr launch crashes were: the APK built and verified fine,
// then crashed on the phone. The check runs even when the dex
// step was cache-skipped — the dex files on disk are still the
// real output — and it is milliseconds, so it has no cache of
// its own. It also runs for AAB builds: it sits before package,
// which is the shared packaging step for both formats.
ccb_status ccb_step_dexcheck(ccb_pipe *p) {
  if (p->opts->explain) {
    printf("dexcheck: run (verify dex classes)\n");
    return CCB_OK;
  }
  if (p->opts->dry_run) {
    return CCB_OK;
  }
  // Mirror the dex step's skip case: with no classes or jars at
  // all there is no dex to check. But a project with code sources
  // and no classes is a broken upstream, not a resources-only
  // project: fail loudly instead of letting a class-less dex pass.
  char **class_files = NULL;
  size_t nclass = 0;
  if (ccb_path_is_dir(p->classes_dir)) {
    ccb_collect_files(p->classes_dir, ".class", &class_files, &nclass);
  }
  int has_sources = ccb_dex_has_sources(p);
  if (nclass == 0 && has_sources) {
    ccb_pipe_diag(p, "CCB-E3025", "dexcheck",
      "no compiled classes found, but the project has code sources: "
      "the compile-code output is missing or empty — delete build/ "
      "and rebuild if this persists");
    ccb_free_str_array(class_files, nclass);
    return CCB_ERR_BUILD;
  }
  if (nclass == 0 && p->cfg->jars_count == 0 &&
      p->aar_class_jars_count == 0) {
    ccb_log_info(&p->log, "no classes or jars, skipping dexcheck");
    ccb_free_str_array(class_files, nclass);
    return CCB_OK;
  }
  ccb_free_str_array(class_files, nclass);
  char **missing = NULL;
  size_t nmissing = 0;
  char *err = NULL;
  if (ccb_dexcheck_dir(p->dex_dir, &missing, &nmissing, &err) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3025", "dexcheck", err);
    free(err);
    return CCB_ERR_BUILD;
  }
  if (nmissing > 0) {
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_appendf(&m,
      "dex references %zu class(es) missing from the dex: ", nmissing);
    size_t shown = nmissing < 10 ? nmissing : 10;
    for (size_t i = 0; i < shown; i++) {
      if (i > 0) {
        ccb_str_append(&m, ", ");
      }
      ccb_str_append(&m, missing[i]);
    }
    if (nmissing > shown) {
      ccb_str_appendf(&m, " (+%zu more)", nmissing - shown);
    }
    ccb_str_append(&m,
      "; build stops before packaging — a missing class crashes "
      "at launch with NoClassDefFoundError");
    ccb_pipe_diag(p, "CCB-E3025", "dexcheck", m.buf);
    ccb_str_free(&m);
    ccb_free_str_array(missing, nmissing);
    return CCB_ERR_BUILD;
  }
  ccb_free_str_array(missing, nmissing);
  // Deliberately NOT counted in steps_run: dexcheck has no cache
  // and runs on every build, so counting it would make a no-op
  // rebuild never report "up to date". It is a gate, not a
  // producing step; it only speaks on failure.
  return CCB_OK;
}
