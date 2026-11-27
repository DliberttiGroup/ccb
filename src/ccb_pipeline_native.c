// ccb_pipeline_native: NDK native-libs stage.
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

// ---------- NDK native libs ----------

static const char *ccb_abi_triple(const char *abi) {
  if (strcmp(abi, "arm64-v8a") == 0) {
    return "aarch64-linux-android";
  }
  if (strcmp(abi, "armeabi-v7a") == 0) {
    return "armv7-linux-androideabi";
  }
  if (strcmp(abi, "x86_64") == 0) {
    return "x86_64-linux-android";
  }
  return "i686-linux-android";
}

// Project name sanitized into a C identifier-ish lib name.
static char *ccb_lib_name(const char *project) {
  ccb_str s;
  ccb_str_init(&s);
  for (const char *c = project; *c != '\0'; c++) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_') {
      ccb_str_append_c(&s, *c);
    } else if (*c >= 'A' && *c <= 'Z') {
      ccb_str_append_c(&s, (char)(*c + 32));
    } else {
      ccb_str_append_c(&s, '_');
    }
  }
  if (s.len == 0) {
    ccb_str_append(&s, "app");
  }
  return ccb_str_steal(&s);
}

ccb_status ccb_step_native(ccb_pipe *p) {
  if (!p->cfg->has_ndk) {
    if (p->opts->explain) {
      printf("native: skip (no [ndk] section)\n");
    }
    p->native_will_run = 0;
    return CCB_OK;
  }
  char *libname = ccb_lib_name(p->cfg->name);
  ccb_str sysroot;
  ccb_str_init(&sysroot);
  ccb_str_append(&sysroot, p->sdk->ndk_home);
  ccb_str_append(&sysroot, "/toolchains/llvm/prebuilt/");
  // Host tag: reuse the sdk's clang path to derive it.
  {
    const char *clang = p->sdk->ndk_clang;
    const char *prebuilt = strstr(clang, "/prebuilt/");
    const char *bin = strstr(clang, "/bin/clang");
    char *host = ccb_xstrndup(prebuilt + 10, (size_t)(bin - (prebuilt + 10)));
    ccb_str_append(&sysroot, host);
    ccb_str_append(&sysroot, "/sysroot");
    free(host);
  }
  int any_ran = 0;
  for (size_t ai = 0; ai < p->cfg->ndk_abis_count; ai++) {
    const char *abi = p->cfg->ndk_abis[ai];
    ccb_fp_builder b;
    memset(&b, 0, sizeof(b));
    ccb_fp_add(&b, "ndk", p->cfg->ndk_version);
    ccb_fp_add(&b, "abi", abi);
    ccb_fp_addl(&b, "min_sdk", p->cfg->min_sdk);
    for (size_t i = 0; i < p->cfg->ndk_cflags_count; i++) {
      ccb_fp_add(&b, "cflag", p->cfg->ndk_cflags[i]);
    }
    int failed = 0;
    for (size_t i = 0; i < p->cfg->ndk_sources_count && !failed; i++) {
      if (ccb_fp_add_file(p, &b, p->cfg->ndk_sources[i]) != 0) {
        failed = 1;
      }
    }
    char *fp = NULL;
    const char *old = NULL;
    if (!failed) {
      fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
      char step[64];
      snprintf(step, sizeof(step), "native-%s", abi);
      old = ccb_cache_step_fp(p->cache, step);
    }
    ccb_fp_free(&b);
    char *abi_dir = ccb_path_join3(p->intermediates, "jni", abi);
    char *soname = NULL;
    {
      ccb_str sn;
      ccb_str_init(&sn);
      ccb_str_append(&sn, "lib");
      ccb_str_append(&sn, libname);
      ccb_str_append(&sn, ".so");
      soname = ccb_str_steal(&sn);
    }
    char *so_path = ccb_path_join(abi_dir, soname);
    if (failed) {
      ccb_pipe_diag(p, "CCB-E3012", "ndk", "cannot read native sources");
      free(abi_dir);
      free(soname);
      free(so_path);
      ccb_str_free(&sysroot);
      free(libname);
      return CCB_ERR_BUILD;
    }
    if (p->opts->explain) {
      if (old != NULL && strcmp(old, fp) == 0) {
        printf("native-%s: skip (inputs unchanged)\n", abi);
      } else {
        printf("native-%s: run (native inputs changed)\n", abi);
        any_ran = 1;
      }
      free(fp);
      free(abi_dir);
      free(soname);
      free(so_path);
      continue;
    }
    if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 && ccb_path_exists(so_path)) {
      free(fp);
      free(abi_dir);
      free(soname);
      free(so_path);
      p->steps_skipped++;
      ccb_log_cache(&p->log, "ndk", 1, "native inputs unchanged");
      continue;
    }
    any_ran = 1;
    if (!p->opts->dry_run) {
      ccb_remove_recursive(abi_dir);
      if (ccb_mkdir_p(abi_dir) != CCB_OK) {
        ccb_pipe_diag(p, "CCB-E3016", "ndk", "cannot create native output directory");
        free(fp);
        free(abi_dir);
        free(soname);
        free(so_path);
          ccb_str_free(&sysroot);
        free(libname);
        return CCB_ERR_BUILD;
      }
    }
    // Compile each source, then link shared.
    char *triple = NULL;
    {
      ccb_str t;
      ccb_str_init(&t);
      ccb_str_append(&t, ccb_abi_triple(abi));
      ccb_str_appendf(&t, "%ld", p->cfg->min_sdk);
      triple = ccb_str_steal(&t);
    }
    char **objs = NULL;
    size_t nobjs = 0;
    ccb_status rc = CCB_OK;
    for (size_t si = 0; si < p->cfg->ndk_sources_count && rc == CCB_OK; si++) {
      char *base = ccb_basename(p->cfg->ndk_sources[si]);
      char *dot = strrchr(base, '.');
      if (dot != NULL) {
        *dot = '\0';
      }
      ccb_str obj;
      ccb_str_init(&obj);
      ccb_str_append(&obj, base);
      ccb_str_append(&obj, ".o");
      char *obj_path = ccb_path_join(abi_dir, obj.buf);
      ccb_str_free(&obj);
      free(base);
      CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->ndk_clang);
      ccb_argv_push(&av, "-target");
      ccb_argv_push(&av, triple);
      ccb_argv_push(&av, "--sysroot");
      ccb_argv_push(&av, sysroot.buf);
      ccb_argv_push(&av, "-fPIC");
      ccb_argv_push(&av, "-O2");
      for (size_t fi = 0; fi < p->cfg->ndk_cflags_count; fi++) {
        ccb_argv_push(&av, p->cfg->ndk_cflags[fi]);
      }
      ccb_argv_push(&av, "-c");
      ccb_argv_push(&av, p->cfg->ndk_sources[si]);
      ccb_argv_push(&av, "-o");
      ccb_argv_push(&av, obj_path);
      rc = ccb_run_tool(p, "ndk", "CCB-E3012", av.v, NULL,
        "check the native sources for errors");
      free(av.v);
      if (rc == CCB_OK) {
        if (nobjs == 0) {
          objs = ccb_xmalloc(p->cfg->ndk_sources_count * sizeof(*objs));
        }
        objs[nobjs++] = obj_path;
      } else {
        free(obj_path);
      }
    }
    free(triple);
    if (rc == CCB_OK) {
      CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->ndk_clang);
      ccb_argv_push(&av, "-target");
      {
        ccb_str t;
        ccb_str_init(&t);
        ccb_str_append(&t, ccb_abi_triple(abi));
        ccb_str_appendf(&t, "%ld", p->cfg->min_sdk);
        ccb_argv_push(&av, t.buf);
        ccb_str_free(&t);
      }
      ccb_argv_push(&av, "--sysroot");
      ccb_argv_push(&av, sysroot.buf);
      ccb_argv_push(&av, "-shared");
      {
        ccb_str sn;
        ccb_str_init(&sn);
        ccb_str_append(&sn, "-Wl,-soname,");
        ccb_str_append(&sn, soname);
        ccb_argv_push(&av, sn.buf);
        ccb_str_free(&sn);
      }
      for (size_t oi = 0; oi < nobjs; oi++) {
        ccb_argv_push(&av, objs[oi]);
      }
      ccb_argv_push(&av, "-o");
      ccb_argv_push(&av, so_path);
      rc = ccb_run_tool(p, "ndk", "CCB-E3013", av.v, NULL,
        "check the native link for errors (missing symbols, bad objects)");
      free(av.v);
    }
    ccb_free_str_array(objs, nobjs);
    if (rc != CCB_OK) {
      free(fp);
      free(abi_dir);
      free(soname);
      free(so_path);
      ccb_str_free(&sysroot);
      free(libname);
      return CCB_ERR_BUILD;
    }
    char step[64];
    snprintf(step, sizeof(step), "native-%s", abi);
    ccb_cache_set_step(p->cache, step, fp);
    free(fp);
    p->cache_dirty = 1;
    p->steps_run++;
    free(abi_dir);
    free(soname);
    free(so_path);
  }
  p->native_will_run = any_ran;
  ccb_str_free(&sysroot);
  free(libname);
  return CCB_OK;
}
