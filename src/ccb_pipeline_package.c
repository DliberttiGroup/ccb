// ccb_pipeline_package: package stage (APK/AAB writer).
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

// ---------- package ----------

static int ccb_name_seen(char **names, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

// Map a link-output entry name to its AAB archive name. APK builds keep
// names verbatim and never call this.
static char *ccb_aab_entry_name(const char *name) {
  if (strcmp(name, "AndroidManifest.xml") == 0) {
    return ccb_xstrdup("base/manifest/AndroidManifest.xml");
  }
  if (strcmp(name, "resources.pb") == 0) {
    return ccb_xstrdup("base/resources.pb");
  }
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "base/");
  ccb_str_append(&s, name);
  return ccb_str_steal(&s);
}

// ---------- package ----------

static void ccb_pkg_ctx_init(ccb_pkg_ctx *c, ccb_pipe *p, int is_aab) {
  memset(c, 0, sizeof(*c));
  c->p = p;
  c->is_aab = is_aab;
  // AAB layout: link output goes under base/ (manifest at
  // base/manifest/AndroidManifest.xml, resources.pb at base/resources.pb,
  // res/* at base/res/*), dex at base/dex/, assets at base/assets/,
  // native libs at base/lib/<abi>/, plus BundleConfig.pb at the root.
  // Cache key and intermediates are separate from the APK path.
  c->pkg_key = is_aab ? "package-aab" : "package";
  c->link_src = is_aab ? p->linked_aab : p->linked_apk;
  c->pkg_out = is_aab ? p->unsigned_aab : p->unsigned_apk;
  c->fmt_label = is_aab ? "AAB" : "APK";
  c->asset_prefix = is_aab ? "base/assets/" : "assets/";
  c->so_prefix = is_aab ? "base/lib/" : "lib/";
}

static void ccb_pkg_ctx_free(ccb_pkg_ctx *c) {
  ccb_free_str_array(c->dex_files, c->ndex);
  ccb_free_str_array(c->asset_paths, c->nassets);
  ccb_free_str_array(c->asset_names, c->nassets);
  ccb_free_str_array(c->so_paths, c->nso);
  ccb_free_str_array(c->so_names, c->nso);
}

static void ccb_pkg_push_asset(ccb_pkg_ctx *c, char *path, char *name) {
  if (c->nassets == c->assets_cap) {
    size_t ncap = c->assets_cap == 0 ? 16 : c->assets_cap * 2;
    c->asset_paths = ccb_xrealloc(c->asset_paths, ncap * sizeof(*c->asset_paths));
    c->asset_names = ccb_xrealloc(c->asset_names, ncap * sizeof(*c->asset_names));
    c->assets_cap = ncap;
  }
  c->asset_paths[c->nassets] = path;
  c->asset_names[c->nassets] = name;
  c->nassets++;
}

static void ccb_pkg_push_so(ccb_pkg_ctx *c, char *path, char *name) {
  if (c->nso == c->so_cap) {
    size_t ncap = c->so_cap == 0 ? 16 : c->so_cap * 2;
    c->so_paths = ccb_xrealloc(c->so_paths, ncap * sizeof(*c->so_paths));
    c->so_names = ccb_xrealloc(c->so_names, ncap * sizeof(*c->so_names));
    c->so_cap = ncap;
  }
  c->so_paths[c->nso] = path;
  c->so_names[c->nso] = name;
  c->nso++;
}

// Record one native library, flagging a duplicate archive name. A
// duplicate keeps the first entry; the conflict name is kept for the
// diagnostic (the last duplicate wins, as before).
static void ccb_pkg_add_so(ccb_pkg_ctx *c, char *path, char *name) {
  if (ccb_name_seen(c->so_names, c->nso, name)) {
    c->so_conflict = 1;
    c->so_conflict_name = name;
    free(path);
    return;
  }
  ccb_pkg_push_so(c, path, name);
}

static char *ccb_pkg_asset_name(ccb_pkg_ctx *c, const char *rel) {
  ccb_str an;
  ccb_str_init(&an);
  ccb_str_append(&an, c->asset_prefix);
  ccb_str_append(&an, rel);
  return ccb_str_steal(&an);
}

static char *ccb_pkg_so_name(ccb_pkg_ctx *c, const char *abi,
    const char *base) {
  ccb_str sn;
  ccb_str_init(&sn);
  ccb_str_append(&sn, c->so_prefix);
  ccb_str_append(&sn, abi);
  ccb_str_append_c(&sn, '/');
  ccb_str_append(&sn, base);
  return ccb_str_steal(&sn);
}

// Record one AAR asset, unless the app already provides an asset
// with the same archive name (app assets win). Takes ownership of
// path and name on either outcome.
static void ccb_pkg_add_aar_asset(ccb_pkg_ctx *c, char *path, char *name) {
  if (!ccb_name_seen(c->asset_names, c->nassets, name)) {
    ccb_pkg_push_asset(c, path, name);
    return;
  }
  free(name);
  free(path);
}

// Assets: app assets win over AAR assets with the same archive name.
static void ccb_pkg_collect_assets(ccb_pkg_ctx *c) {
  ccb_pipe *p = c->p;
  if (p->cfg->assets != NULL) {
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(p->cfg->assets, NULL, &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      const char *rel = files[i] + strlen(p->cfg->assets) + 1;
      ccb_pkg_push_asset(c, files[i], ccb_pkg_asset_name(c, rel));
    }
    free(files);
  }
  for (size_t ai = 0; ai < p->aar_assets_count; ai++) {
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(p->aar_assets_dirs[ai], NULL, &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      const char *rel = files[i] + strlen(p->aar_assets_dirs[ai]) + 1;
      ccb_pkg_add_aar_asset(c, files[i], ccb_pkg_asset_name(c, rel));
    }
    free(files);
  }
}

// Native .so files: intermediates/jni/<abi>/*.so + AAR jni.
// Subdirectories of an AAR jni dir are ABIs.
static void ccb_pkg_collect_so(ccb_pkg_ctx *c) {
  ccb_pipe *p = c->p;
  for (size_t ai = 0; ai < p->cfg->ndk_abis_count; ai++) {
    char *abi_dir = ccb_path_join3(p->intermediates, "jni", p->cfg->ndk_abis[ai]);
    char **files = NULL;
    size_t nfiles = 0;
    ccb_collect_files(abi_dir, ".so", &files, &nfiles);
    for (size_t i = 0; i < nfiles; i++) {
      char *base = ccb_basename(files[i]);
      char *sname = ccb_pkg_so_name(c, p->cfg->ndk_abis[ai], base);
      free(base);
      ccb_pkg_add_so(c, files[i], sname);
    }
    free(files);
    free(abi_dir);
  }
  for (size_t ai = 0; ai < p->aar_jni_count; ai++) {
    char **all = NULL;
    size_t nall = 0;
    ccb_collect_files(p->aar_jni_dirs[ai], ".so", &all, &nall);
    for (size_t i = 0; i < nall; i++) {
      // Path: <jni>/<abi>/<file>.so -> abi is the parent dir name.
      char *parent = ccb_dirname(all[i]);
      char *abi = ccb_basename(parent);
      free(parent);
      char *base = ccb_basename(all[i]);
      char *sname = ccb_pkg_so_name(c, abi, base);
      free(base);
      free(abi);
      ccb_pkg_add_so(c, all[i], sname);
    }
    free(all);
  }
}

// Fingerprint the packaging inputs: link output, dex files, assets,
// and native libraries (archive names + file contents). In
// explain/dry-run mode on a fresh tree the link output does not exist
// yet (link never executes in planning modes); skipping it keeps
// planning working, and a missing link output still fails the real
// build here with a clear error. Sets *fp_out (owned, NULL on
// failure) and *old_out (borrowed cache fingerprint, NULL on
// failure). Returns 0 on success, -1 when an input cannot be read.
static int ccb_pkg_fingerprint(ccb_pkg_ctx *c, char **fp_out,
    const char **old_out) {
  ccb_pipe *p = c->p;
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  int failed = 0;
  if ((p->opts->explain || p->opts->dry_run) &&
      !ccb_path_exists(c->link_src)) {
    // Skip: planning only.
  } else if (ccb_fp_add_file(p, &b, c->link_src) != 0) {
    failed = 1;
  }
  for (size_t i = 0; i < c->ndex && !failed; i++) {
    char *base = ccb_basename(c->dex_files[i]);
    ccb_fp_add(&b, base, "");
    free(base);
    if (ccb_fp_add_file(p, &b, c->dex_files[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < c->nassets && !failed; i++) {
    ccb_fp_add(&b, c->asset_names[i], "");
    if (ccb_fp_add_file(p, &b, c->asset_paths[i]) != 0) {
      failed = 1;
    }
  }
  for (size_t i = 0; i < c->nso && !failed; i++) {
    ccb_fp_add(&b, c->so_names[i], "");
    if (ccb_fp_add_file(p, &b, c->so_paths[i]) != 0) {
      failed = 1;
    }
  }
  *fp_out = NULL;
  *old_out = NULL;
  if (!failed) {
    *fp_out = ccb_cache_fingerprint((const char **)b.keys,
      (const char **)b.vals, b.count);
    *old_out = ccb_cache_step_fp(p->cache, c->pkg_key);
  }
  ccb_fp_free(&b);
  return failed ? -1 : 0;
}

// Copy the link output's entries into the package, renaming them for
// the AAB base/ layout. The archive must already be open.
static ccb_status ccb_pkg_write_linked(ccb_pkg_ctx *c, ccb_zip_writer *w,
    ccb_zip_archive *linked) {
  ccb_status rc = CCB_OK;
  for (size_t i = 0; i < linked->count && rc == CCB_OK; i++) {
    const ccb_zip_entry *e = &linked->entries[i];
    size_t nl = strlen(e->name);
    if (nl == 0 || e->name[nl - 1] == '/') {
      continue;
    }
    void *data = NULL;
    size_t len = 0;
    if (ccb_zip_read_entry(linked, e, &data, &len) != CCB_OK) {
      rc = CCB_ERR_BUILD;
      break;
    }
    char *arc_name = c->is_aab ? ccb_aab_entry_name(e->name) : ccb_xstrdup(e->name);
    if (ccb_zip_writer_add_data(w, arc_name, data, len) != CCB_OK) {
      rc = CCB_ERR_BUILD;
    }
    free(arc_name);
    free(data);
  }
  return rc;
}

// Read one file and add it to the package under arc_name.
static ccb_status ccb_pkg_write_file(ccb_zip_writer *w,
    const char *arc_name, const char *path) {
  char *text = NULL;
  size_t tlen = 0;
  if (ccb_read_file(path, &text, &tlen) != CCB_OK) {
    return CCB_ERR_BUILD;
  }
  ccb_status rc = ccb_zip_writer_add_data(w, arc_name, text, tlen);
  free(text);
  return rc;
}

// Write the unsigned package: link output entries, dex files, assets,
// native libraries, and (AAB only) BundleConfig.pb.
static ccb_status ccb_pkg_write(ccb_pkg_ctx *c) {
  ccb_pipe *p = c->p;
  ccb_zip_writer w;
  if (ccb_zip_writer_init(&w, c->pkg_out) != CCB_OK) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "cannot create unsigned ");
    ccb_str_append(&msg, c->fmt_label);
    ccb_pipe_diag(p, "CCB-E3016", "package", msg.buf);
    ccb_str_free(&msg);
    return CCB_ERR_BUILD;
  }
  ccb_zip_archive *linked = NULL;
  if (ccb_zip_open(c->link_src, &linked) != CCB_OK) {
    ccb_pipe_diag(p, "CCB-E3007", "package", "cannot open linked resources");
    ccb_zip_writer_abort(&w);
    return CCB_ERR_BUILD;
  }
  ccb_status wrc = ccb_pkg_write_linked(c, &w, linked);
  ccb_zip_close(linked);
  for (size_t i = 0; i < c->ndex && wrc == CCB_OK; i++) {
    char *base = ccb_basename(c->dex_files[i]);
    char *arc_name = base;
    ccb_str dex_name;
    if (c->is_aab) {
      ccb_str_init(&dex_name);
      ccb_str_append(&dex_name, "base/dex/");
      ccb_str_append(&dex_name, base);
      arc_name = dex_name.buf;
    }
    if (ccb_pkg_write_file(&w, arc_name, c->dex_files[i]) != CCB_OK) {
      wrc = CCB_ERR_BUILD;
    }
    if (c->is_aab) {
      ccb_str_free(&dex_name);
    }
    free(base);
  }
  for (size_t i = 0; i < c->nassets && wrc == CCB_OK; i++) {
    if (ccb_pkg_write_file(&w, c->asset_names[i], c->asset_paths[i]) != CCB_OK) {
      wrc = CCB_ERR_BUILD;
    }
  }
  for (size_t i = 0; i < c->nso && wrc == CCB_OK; i++) {
    if (ccb_pkg_write_file(&w, c->so_names[i], c->so_paths[i]) != CCB_OK) {
      wrc = CCB_ERR_BUILD;
    }
  }
  if (c->is_aab && wrc == CCB_OK) {
    uint8_t *bc = NULL;
    size_t bc_len = 0;
    if (ccb_bundle_config_encode("1.0.0", &bc, &bc_len) != CCB_OK ||
        ccb_zip_writer_add_data(&w, "BundleConfig.pb", bc, bc_len) != CCB_OK) {
      wrc = CCB_ERR_BUILD;
    }
    free(bc);
  }
  if (wrc != CCB_OK) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "cannot write ");
    ccb_str_append(&msg, c->fmt_label);
    ccb_str_append(&msg, " entries");
    ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
    ccb_str_free(&msg);
    ccb_zip_writer_abort(&w);
    return CCB_ERR_BUILD;
  }
  if (ccb_zip_writer_finish(&w) != CCB_OK) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "cannot finish ");
    ccb_str_append(&msg, c->fmt_label);
    ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
    ccb_str_free(&msg);
    return CCB_ERR_BUILD;
  }
  return CCB_OK;
}

// Shared APK/AAB packaging: collect inputs, fingerprint, explain or
// cache-skip, then write the unsigned package.
static ccb_status ccb_package_format(ccb_pipe *p, int is_aab) {
  ccb_pkg_ctx c;
  ccb_pkg_ctx_init(&c, p, is_aab);
  if (ccb_path_is_dir(p->dex_dir)) {
    ccb_collect_files(p->dex_dir, ".dex", &c.dex_files, &c.ndex);
  }
  ccb_pkg_collect_assets(&c);
  ccb_pkg_collect_so(&c);
  char *fp = NULL;
  const char *old = NULL;
  int failed = ccb_pkg_fingerprint(&c, &fp, &old) != 0;
  ccb_status rc = CCB_OK;
  if (c.so_conflict) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "duplicate native library in ");
    ccb_str_append(&msg, c.fmt_label);
    ccb_str_append(&msg, ": ");
    ccb_str_append(&msg, c.so_conflict_name);
    ccb_pipe_diag(p, "CCB-E3007", "package", msg.buf);
    ccb_str_free(&msg);
    free(c.so_conflict_name);
    free(fp);
    rc = CCB_ERR_BUILD;
    goto done;
  }
  if (failed) {
    ccb_pipe_diag(p, "CCB-E3007", "package", "cannot read packaging inputs");
    free(fp);
    rc = CCB_ERR_BUILD;
    goto done;
  }
  if (p->opts->explain) {
    if (p->link_will_run || p->code_will_run || p->dex_will_run || p->native_will_run) {
      printf("package: run (upstream outputs will change)\n");
      p->package_will_run = 1;
    } else if (old != NULL && strcmp(old, fp) == 0) {
      printf("package: skip (inputs unchanged)\n");
      p->package_will_run = 0;
    } else {
      printf("package: run (packaging inputs changed)\n");
      p->package_will_run = 1;
    }
    free(fp);
    goto done;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(c.pkg_out)) {
    free(fp);
    p->package_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "package", 1, "packaging inputs unchanged");
    goto done;
  }
  p->package_will_run = 1;
  if (!p->opts->dry_run) {
    if (ccb_pkg_write(&c) != CCB_OK) {
      free(fp);
      rc = CCB_ERR_BUILD;
      goto done;
    }
    ccb_cache_set_step(p->cache, c.pkg_key, fp);
    free(fp);
    p->cache_dirty = 1;
    p->steps_run++;
  } else {
    free(fp);
    p->steps_run++;
  }
done:
  ccb_pkg_ctx_free(&c);
  return rc;
}

ccb_status ccb_step_package(ccb_pipe *p) {
  return ccb_package_format(p, 0);
}

ccb_status ccb_step_package_aab(ccb_pipe *p) {
  return ccb_package_format(p, 1);
}
