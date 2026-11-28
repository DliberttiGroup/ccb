// ccb_pipeline_code: kotlinc (+ javac) + d8 + d8 heap sizing +
// stdlib dex cache stages. Split from ccb_pipeline.c.

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

// ---------- kotlinc (+ javac) ----------

char *ccb_java_home(ccb_pipe *p) {
  char *bin = ccb_dirname(p->sdk->java);
  char *home = ccb_dirname(bin);
  free(bin);
  return home;
}

// Env for JVM tools: heap fallback + JAVA_HOME derived from the resolved
// java. The JAVA_TOOL_OPTIONS cap only binds tools whose launcher sets
// no heap of its own (R8, javac, keytool): the d8 wrapper defaults to
// -Xmx2G, so d8 gets a sized -J flag instead (see ccb_d8_heap_mb),
// and the kotlinc/apksigner wrappers set their own.
// PATH is also prefixed with the JDK bin dir: the build-tools wrapper
// scripts (d8, apksigner) exec plain `java` from PATH and ignore
// JAVA_HOME, so without this they fail on machines where java is not
// already on PATH.
char **ccb_jvm_env(ccb_pipe *p, ccb_arena *ar) {
  char **env = ccb_arena_alloc(ar, 4 * sizeof(*env));
  env[0] = ccb_arena_strdup(ar, "JAVA_TOOL_OPTIONS=-Xmx1g");
  ccb_str jh;
  ccb_str_init(&jh);
  ccb_str_append(&jh, "JAVA_HOME=");
  char *home = ccb_java_home(p);
  ccb_str_append(&jh, home);
  free(home);
  env[1] = ccb_arena_strdup(ar, jh.buf);
  ccb_str_free(&jh);
  char *bin = ccb_dirname(p->sdk->java);
  ccb_str path;
  ccb_str_init(&path);
  ccb_str_append(&path, "PATH=");
  ccb_str_append(&path, bin);
  const char *old_path = getenv("PATH");
  if (old_path != NULL) {
    ccb_str_append_c(&path, ':');
    ccb_str_append(&path, old_path);
  }
  free(bin);
  env[2] = ccb_arena_strdup(ar, path.buf);
  ccb_str_free(&path);
  env[3] = NULL;
  return env;
}

static char *ccb_classes_fingerprint(ccb_pipe *p) {
  char **files = NULL;
  size_t nfiles = 0;
  if (!ccb_path_is_dir(p->classes_dir)) {
    return ccb_xstrdup("");
  }
  if (ccb_collect_files(p->classes_dir, ".class", &files, &nfiles) != CCB_OK) {
    return NULL;
  }
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  int failed = 0;
  for (size_t i = 0; i < nfiles; i++) {
    if (ccb_fp_add_file(p, &b, files[i]) != 0) {
      failed = 1;
      break;
    }
  }
  ccb_free_str_array(files, nfiles);
  if (failed) {
    ccb_fp_free(&b);
    return NULL;
  }
  char *fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
  ccb_fp_free(&b);
  return fp;
}

// True when path is a whole .class file: non-empty with the
// CAFEBABE magic. A truncated class file fails, so a compile-code
// skip never trusts it.
static int ccb_class_file_valid(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 8) {
    return 0;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return 0;
  }
  unsigned char head[4];
  size_t n = fread(head, 1, sizeof(head), f);
  fclose(f);
  return n == sizeof(head) && head[0] == 0xCA && head[1] == 0xFE &&
    head[2] == 0xBA && head[3] == 0xBE;
}

// True when the classes dir holds at least one whole .class file.
// Existence of the dir alone is not enough: a dir left behind
// after its classes were deleted must not validate a skip.
static int ccb_classes_dir_valid(ccb_pipe *p, char **rjava, size_t nrjava) {
  const char *dir = p->classes_dir;
  if (!ccb_path_is_dir(dir)) {
    return 0;
  }
  char **files = NULL;
  size_t n = 0;
  if (ccb_collect_files(dir, ".class", &files, &n) != CCB_OK) {
    return 0;
  }
  int ok = n > 0;
  for (size_t i = 0; i < n && ok; i++) {
    if (!ccb_class_file_valid(files[i])) {
      ok = 0;
    }
  }
  ccb_free_str_array(files, n);
  // Every generated R.java must have its R.class. The step wipes the
  // classes dir before compiling, so a failed javac leaves it without
  // R.class; a later skip must not trust a dir that lost it, or every
  // build after would ship an APK without R.class and never notice.
  // The R.java path is <gen>/<rel>.java; javac writes <classes>/<rel>.class.
  size_t gen_len = strlen(p->gen_dir);
  for (size_t i = 0; i < nrjava && ok; i++) {
    const char *rel = rjava[i];
    if (strncmp(rel, p->gen_dir, gen_len) == 0 && rel[gen_len] == '/') {
      rel += gen_len + 1;
    }
    size_t rel_len = strlen(rel);
    if (rel_len < 6 || strcmp(rel + rel_len - 5, ".java") != 0) {
      ok = 0;
      continue;
    }
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, dir);
    ccb_str_append_c(&s, '/');
    ccb_str_append_n(&s, rel, rel_len - 5);
    ccb_str_append(&s, ".class");
    char *want = ccb_str_steal(&s);
    if (!ccb_path_exists(want)) {
      ok = 0;
    }
    free(want);
  }
  return ok;
}

ccb_status ccb_step_code(ccb_pipe *p) {
  // Declared first: an early goto below must never jump over a
  // cleanup-attributed declaration, or cleanup would run on garbage.
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  char **kt_files = NULL;
  size_t nkt = 0;
  char **java_files = NULL;
  size_t njava = 0;
  char **rjava_files = NULL;
  size_t nrjava = 0;
  if (p->cfg->kotlin_dir != NULL) {
    ccb_collect_files(p->cfg->kotlin_dir, ".kt", &kt_files, &nkt);
  }
  if (p->cfg->java_dir != NULL) {
    ccb_collect_files(p->cfg->java_dir, ".java", &java_files, &njava);
  }
  // Vendored Java roots outside the project tree ([paths] java_extra):
  // compiled alongside java_dir, by kotlinc (mixed sources) and javac.
  for (size_t d = 0; d < p->cfg->java_extra_dirs_count; d++) {
    char **found = NULL;
    size_t nfound = 0;
    if (ccb_collect_files(p->cfg->java_extra_dirs[d], ".java", &found,
        &nfound) != CCB_OK) {
      continue;
    }
    java_files = ccb_xrealloc(java_files,
      (njava + nfound) * sizeof(*java_files));
    for (size_t i = 0; i < nfound; i++) {
      java_files[njava++] = found[i];
    }
    free(found);
  }
  if (ccb_path_is_dir(p->gen_dir)) {
    ccb_collect_files(p->gen_dir, ".java", &rjava_files, &nrjava);
  }
  if (nkt == 0 && njava == 0 && nrjava == 0) {
    if (p->opts->explain) {
      printf("compile-code: skip (no sources)\n");
    }
    ccb_log_info(&p->log, "no sources, skipping compile-code");
    p->code_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "compile-code", 1, "no sources to compile");
    goto code_done;
  }
  // Classpath: android.jar + jars + AAR classes.jars.
  ccb_str cp;
  ccb_str_init(&cp);
  ccb_str_append(&cp, p->sdk->android_jar);
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    ccb_str_append_c(&cp, ':');
    ccb_str_append(&cp, p->cfg->jars[i]);
  }
  for (size_t i = 0; i < p->aar_class_jars_count; i++) {
    ccb_str_append_c(&cp, ':');
    ccb_str_append(&cp, p->aar_class_jars[i]);
  }
  char *cp_str = ccb_str_steal(&cp);
  {
    ccb_fp_builder b;
    memset(&b, 0, sizeof(b));
    ccb_fp_add(&b, "kotlinc", p->sdk->kotlin_version);
    ccb_fp_add(&b, "jvm_target", p->cfg->jvm_target);
    ccb_fp_add(&b, "classpath", cp_str);
    int failed = 0;
    // Classpath contents, not just paths: a changed dependency jar
    // (including a rebuilt library module) must recompile app code,
    // or stale app classes could reference changed library APIs. Jars
    // are hashed only when present: in dry-run on a fresh tree the
    // extracted AAR classes do not exist yet, and their absence must
    // not fail planning (a missing jar still fails the build at dex
    // time with a clear error).
    for (size_t i = 0; i < p->cfg->jars_count && !failed; i++) {
      if (ccb_path_exists(p->cfg->jars[i]) &&
          ccb_fp_add_file(p, &b, p->cfg->jars[i]) != 0) {
        failed = 1;
      }
    }
    for (size_t i = 0; i < p->aar_class_jars_count && !failed; i++) {
      if (ccb_path_exists(p->aar_class_jars[i]) &&
          ccb_fp_add_file(p, &b, p->aar_class_jars[i]) != 0) {
        failed = 1;
      }
    }
    ccb_fp_add_plugins(p, &b, &failed);
    for (size_t i = 0; i < nkt && !failed; i++) {
      if (ccb_fp_add_file(p, &b, kt_files[i]) != 0) {
        failed = 1;
      }
    }
    for (size_t i = 0; i < njava && !failed; i++) {
      if (ccb_fp_add_file(p, &b, java_files[i]) != 0) {
        failed = 1;
      }
    }
    for (size_t i = 0; i < nrjava && !failed; i++) {
      if (ccb_fp_add_file(p, &b, rjava_files[i]) != 0) {
        failed = 1;
      }
    }
    char *fp = NULL;
    const char *old = NULL;
    if (!failed) {
      fp = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
      old = ccb_cache_step_fp(p->cache, "compile-code");
    }
    ccb_fp_free(&b);
    if (failed) {
      ccb_pipe_diag(p, "CCB-E3004", "kotlinc", "cannot read source files");
      free(cp_str);
      goto code_fail;
    }
    if (p->opts->explain) {
      if (p->link_will_run) {
        printf("compile-code: run (link outputs will change)\n");
        p->code_will_run = 1;
      } else if (old != NULL && strcmp(old, fp) == 0 &&
          ccb_classes_dir_valid(p, rjava_files, nrjava)) {
        printf("compile-code: skip (inputs unchanged)\n");
        p->code_will_run = 0;
      } else if (old != NULL && strcmp(old, fp) == 0) {
        printf("compile-code: run (compiled classes missing or invalid)\n");
        p->code_will_run = 1;
      } else {
        printf("compile-code: run (source inputs changed)\n");
        p->code_will_run = 1;
      }
      free(cp_str);
      free(fp);
      goto code_done;
    }
    // A fingerprint match alone never skips: the compiled classes
    // must still be on disk and whole, or the step re-runs. Without
    // this, deleting build/intermediates/classes reported a
    // successful "up to date" build from a stale APK.
    if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 &&
        ccb_classes_dir_valid(p, rjava_files, nrjava)) {
      free(cp_str);
      free(fp);
      p->code_will_run = 0;
      p->steps_skipped++;
      ccb_log_cache(&p->log, "compile-code", 1, "source inputs unchanged");
      goto code_done;
    }
    p->code_will_run = 1;
    if (!p->opts->dry_run) {
      ccb_remove_recursive(p->classes_dir);
      if (ccb_mkdir_p(p->classes_dir) != CCB_OK) {
        ccb_pipe_diag(p, "CCB-E3016", "kotlinc", "cannot create classes directory");
        free(cp_str);
        free(fp);
        goto code_fail;
      }
    }
    // kotlinc: Kotlin + R.java + Java sources (kotlinc resolves Java
    // symbols from sources passed on the command line). Skipped for
    // Java-only projects: the SDK resolve leaves kotlinc NULL there
    // instead of failing, and kotlinc must never run with no .kt
    // sources (javac compiles the Java sources alone).
    if (nkt > 0 && p->sdk->kotlinc != NULL) {
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->kotlinc);
      ccb_argv_push(&av, "-jvm-target");
      ccb_argv_push(&av, p->cfg->jvm_target);
      ccb_argv_push(&av, "-d");
      ccb_argv_push(&av, p->classes_dir);
      ccb_argv_push(&av, "-cp");
      ccb_argv_push(&av, cp_str);
      ccb_push_plugin_args(p, &av);
      for (size_t i = 0; i < nkt; i++) {
        ccb_argv_push(&av, kt_files[i]);
      }
      for (size_t i = 0; i < nrjava; i++) {
        ccb_argv_push(&av, rjava_files[i]);
      }
      for (size_t i = 0; i < njava; i++) {
        ccb_argv_push(&av, java_files[i]);
      }
      char **env = ccb_jvm_env(p, ar);
      ccb_status rc = ccb_run_kotlinc(p, av.v, env,
        "check the Kotlin/Java sources for errors");
      free(av.v);
      if (rc != CCB_OK) {
        free(cp_str);
        free(fp);
        goto code_fail;
      }
    }
    // javac: compile the Java sources AND the generated R.java.
    // kotlinc resolves R symbols from the sources but emits no
    // R.class files for them (bug-aar: library R classes such as
    // androidx.recyclerview.R$attr were missing at runtime), so
    // javac must compile R.java too.
    if (njava > 0 || nrjava > 0) {
      ccb_str jcp;
      ccb_str_init(&jcp);
      ccb_str_append(&jcp, p->classes_dir);
      ccb_str_append_c(&jcp, ':');
      ccb_str_append(&jcp, cp_str);
      char *jcp_str = ccb_str_steal(&jcp);
      ccb_argv av;
      memset(&av, 0, sizeof(av));
      av.ar = ar;
      ccb_argv_push(&av, p->sdk->javac);
      // UTF-8 source encoding regardless of the host locale: Java
      // files are written in UTF-8 (a non-ASCII comment must not
      // fail the build on a non-UTF-8 machine).
      ccb_argv_push(&av, "-encoding");
      ccb_argv_push(&av, "UTF-8");
      ccb_argv_push(&av, "-source");
      ccb_argv_push(&av, p->cfg->jvm_target);
      ccb_argv_push(&av, "-target");
      ccb_argv_push(&av, p->cfg->jvm_target);
      ccb_argv_push(&av, "-cp");
      ccb_argv_push(&av, jcp_str);
      ccb_argv_push(&av, "-d");
      ccb_argv_push(&av, p->classes_dir);
      for (size_t i = 0; i < nrjava; i++) {
        ccb_argv_push(&av, rjava_files[i]);
      }
      for (size_t i = 0; i < njava; i++) {
        ccb_argv_push(&av, java_files[i]);
      }
      char **env = ccb_jvm_env(p, ar);
      ccb_status rc = ccb_run_tool(p, "javac", "CCB-E3005", av.v, env,
        "check the Java sources for errors");
      free(av.v);
      free(jcp_str);
      if (rc != CCB_OK) {
        free(cp_str);
        free(fp);
        goto code_fail;
      }
    }
    free(cp_str);
    // The compilers reported success but produced no classes: the
    // sources declare nothing compilable (or the output was lost).
    // Fail here, at the step that owns the output, instead of
    // letting dex silently skip and packaging an APK with no code.
    // Planning modes are exempt: in dry-run the compilers never
    // actually run, so no classes exist yet.
    if (!p->opts->dry_run && !ccb_classes_dir_valid(p, rjava_files, nrjava)) {
      ccb_pipe_diag(p, "CCB-E3004", "kotlinc",
        "compilation produced no .class files from the project sources");
      free(fp);
      goto code_fail;
    }
    ccb_cache_set_step(p->cache, "compile-code", fp);
    free(fp);
    p->cache_dirty = 1;
    p->steps_run++;
  }
code_done:
  free(p->classes_fp);
  p->classes_fp = ccb_classes_fingerprint(p);
  ccb_free_str_array(kt_files, nkt);
  ccb_free_str_array(java_files, njava);
  ccb_free_str_array(rjava_files, nrjava);
  return CCB_OK;
code_fail:
  ccb_free_str_array(kt_files, nkt);
  ccb_free_str_array(java_files, njava);
  ccb_free_str_array(rjava_files, nrjava);
  return CCB_ERR_BUILD;
}

// ---------- d8 ----------

// True when the project already feeds kotlin-stdlib to dex itself
// (a [deps] jar or a Maven-resolved jar), so auto-adding it would
// make d8/R8 reject the duplicate classes.
int ccb_dex_has_stdlib(ccb_pipe *p) {
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    const char *base = strrchr(p->cfg->jars[i], '/');
    base = base != NULL ? base + 1 : p->cfg->jars[i];
    // kotlinc's own jar is "kotlin-stdlib.jar"; Maven's is
    // "kotlin-stdlib-<version>.jar" — both supply the same classes.
    if (strcmp(base, "kotlin-stdlib.jar") == 0 ||
        strncmp(base, "kotlin-stdlib-", 14) == 0) {
      return 1;
    }
  }
  return 0;
}

// True when the project has Kotlin sources.
static int ccb_dex_has_kotlin(ccb_pipe *p) {
  if (p->cfg->kotlin_dir == NULL) {
    return 0;
  }
  char **kt = NULL;
  size_t nkt = 0;
  ccb_collect_files(p->cfg->kotlin_dir, ".kt", &kt, &nkt);
  ccb_free_str_array(kt, nkt);
  return nkt > 0;
}

// The kotlinc stdlib jar ($KOTLIN_HOME/lib/kotlin-stdlib.jar) to add
// as a dex program input, or NULL when the project has no Kotlin
// sources, already supplies the stdlib, or kotlinc is unusable.
// kotlinc output references the runtime, but nothing ever fed it to
// d8/R8 as a program input (bug-stdlib: a no-deps Kotlin project
// shipped an APK that crashed on launch with NoClassDefFoundError:
// kotlin.jvm.internal.Intrinsics). Auto-freed by the caller.
char *ccb_dex_stdlib(ccb_pipe *p) {
  if (!ccb_dex_has_kotlin(p) || ccb_dex_has_stdlib(p)) {
    return NULL;
  }
  if (p->sdk->kotlin_home == NULL) {
    return NULL;
  }
  char *stdlib = ccb_path_join3(p->sdk->kotlin_home, "lib", "kotlin-stdlib.jar");
  if (stdlib == NULL || access(stdlib, R_OK) != 0) {
    free(stdlib);
    return NULL;
  }
  return stdlib;
}

// ---------- d8 heap sizing (opt-d8-heap) ----------
//
// The d8 wrapper script defaults to -Xmx2G, and that command-line
// flag beats CCB's JAVA_TOOL_OPTIONS=-Xmx1g (the JVM applies
// JAVA_TOOL_OPTIONS first, command-line options last, and the last
// -Xmx wins — verified with -XX:+PrintFlagsFinal). So every d8 run
// used to reserve 2G, and four parallel builds committed 8G on a
// 6G VM. The wrapper takes a -J option instead: a leading
// -JXmx<N>m becomes the java -Xmx<N>m and suppresses the 2G
// default, so CCB sizes the heap from the actual dex inputs.
//
// Formula: heap = 256MB + 16MB per MiB of program inputs (app
// .class files + [deps] jars + AAR classes.jars + the stdlib input,
// cached .dex or jar), capped at 1024MB. Floor evidence (2026-11-25,
// build-tools 36.0.0): d8 succeeds at 128m on the Showcase warm
// inputs (2.6MB in, dex byte-identical to the 2G run) and on the
// stdlib jar alone (1.9MB), and at 256m on 10,000 synthetic classes
// + stdlib dex (8.2MB in) with no time penalty vs 1024m — so the
// 256MB floor is 2x the proven minimum for a normal build, and the
// cap keeps four parallel d8 runs at 4G. The heap never changes the
// dex output, so it is not part of the dex fingerprint. R8 is
// deliberately untouched: whole-program shrinking does not follow
// this input-size rule, and its 1G JAVA_TOOL_OPTIONS cap already
// equals the cap here.

// Heap in MB for a dex run over input_bytes of program inputs:
// floor + 16MB per whole MiB, capped. Pure and deterministic, so
// tests can call it directly.
long ccb_d8_heap_mb(uint64_t input_bytes) {
  uint64_t mib = input_bytes / (1024 * 1024);
  if (mib >= (CCB_D8_HEAP_CAP_MB - CCB_D8_HEAP_FLOOR_MB) / CCB_D8_HEAP_MB_PER_MIB) {
    return CCB_D8_HEAP_CAP_MB;
  }
  return CCB_D8_HEAP_FLOOR_MB + (long)(mib * CCB_D8_HEAP_MB_PER_MIB);
}

// Size of one file, or 0 when it is missing or unreadable. Missing
// inputs are 0, not an error: in dry-run mode the classes and jars
// do not exist yet, and the heap just falls back toward the floor.
static uint64_t ccb_dex_file_bytes(const char *path) {
  struct stat st;
  if (path == NULL || stat(path, &st) != 0 || st.st_size < 0) {
    return 0;
  }
  return (uint64_t)st.st_size;
}

// Total bytes of everything d8 will read as program inputs: the app
// .class files, [deps] jars, AAR classes.jars, and the stdlib input
// (cached .dex when one is in use, else the jar).
uint64_t ccb_dex_input_bytes(ccb_pipe *p, const char *stdlib_input) {
  uint64_t total = 0;
  char **cfiles = NULL;
  size_t ncfiles = 0;
  if (ccb_path_is_dir(p->classes_dir)) {
    ccb_collect_files(p->classes_dir, ".class", &cfiles, &ncfiles);
  }
  for (size_t i = 0; i < ncfiles; i++) {
    total += ccb_dex_file_bytes(cfiles[i]);
  }
  ccb_free_str_array(cfiles, ncfiles);
  for (size_t i = 0; i < p->cfg->jars_count; i++) {
    total += ccb_dex_file_bytes(p->cfg->jars[i]);
  }
  for (size_t i = 0; i < p->aar_class_jars_count; i++) {
    total += ccb_dex_file_bytes(p->aar_class_jars[i]);
  }
  total += ccb_dex_file_bytes(stdlib_input);
  return total;
}

// ---------- stdlib dex cache (opt-dex / opt-cache) ----------
//
// d8 on the full kotlin-stdlib jar takes ~40s on this VM, on every
// clean build, even though the result never changes. Dex it once into
// the global cache ($XDG_CACHE_HOME/ccb/dex-stdlib, else
// ~/.cache/ccb/dex-stdlib — the same root as the Maven cache, so it
// survives `ccb clean`) and pass the cached .dex to d8 as a program
// input instead of the jar: d8 merges .dex inputs with the app
// classes (verified by hand: a merged dex holds both Intrinsics and
// the app class, in ~8s instead of ~42s).
//
// The entry is one file, <key>.dex, published by atomic rename, so a
// reader never sees a partial file. The key is a fingerprint of the
// stdlib jar hash + min-api + build-tools version, so a kotlinc
// upgrade, a min-api change, or a build-tools upgrade all miss and
// re-dex. A missing or corrupt entry falls back to the jar: the
// build never fails because of the cache, it just goes slow again.
// R8 is excluded: it takes the stdlib as a program-input JAR, not
// dex, so release + minify keeps the jar path exactly as before.

// Cache key for a dexed stdlib. NULL when the jar cannot be hashed.
static char *ccb_stdlib_dex_key(ccb_pipe *p, const char *stdlib) {
  char *hash = ccb_cache_hash_file(stdlib);
  if (hash == NULL) {
    return NULL;
  }
  ccb_fp_builder b;
  memset(&b, 0, sizeof(b));
  ccb_fp_add(&b, "stdlib", hash);
  ccb_fp_addl(&b, "min_sdk", p->cfg->min_sdk);
  ccb_fp_add(&b, "build_tools", p->sdk->build_tools_version);
  char *key = ccb_cache_fingerprint((const char **)b.keys, (const char **)b.vals, b.count);
  ccb_fp_free(&b);
  free(hash);
  return key;
}

// Global cache dir for dexed stdlibs, or NULL when no cache base
// exists. The caller frees the result.
static char *ccb_stdlib_dex_dir(void) {
  char *root = ccb_net_cache_root();
  if (root == NULL) {
    return NULL;
  }
  char *dir = ccb_path_join(root, "dex-stdlib");
  free(root);
  return dir;
}

// True when path is a whole dex file: the dex magic plus a header
// file_size that matches the file on disk. A truncated or foreign
// file fails, so it is re-dexed instead of fed to d8.
int ccb_dex_file_valid(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return 0;
  }
  unsigned char head[112];
  size_t n = fread(head, 1, sizeof(head), f);
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long size = ftell(f);
  fclose(f);
  if (n != sizeof(head) || size < 0) {
    return 0;
  }
  // Dex magic is "dex\n" + 3 version digits + NUL, e.g. "dex\n038".
  // ("dex\n0" as a literal is only 5 bytes, so compare the prefix
  // and the first version digit instead.)
  if (memcmp(head, "dex\n", 4) != 0 || head[4] != '0') {
    return 0;
  }
  uint32_t file_size = (uint32_t)head[32] | ((uint32_t)head[33] << 8) |
    ((uint32_t)head[34] << 16) | ((uint32_t)head[35] << 24);
  return file_size == (uint32_t)size;
}

// Dex the stdlib jar alone into a temp dir, then atomically publish
// the resulting classes.dex as dest. Returns 0 on success, -1 on
// any failure (temp dir removed, dest untouched). The caller falls
// back to the jar on -1, so this never fails a build. A stdlib too
// big for one dex (d8 without --multidex refuses, or emits a
// classes2.dex) also returns -1: the jar path handles it.
static int ccb_stdlib_dex_populate(ccb_pipe *p, const char *stdlib, const char *dest) {
  char *dir = ccb_stdlib_dex_dir();
  if (dir == NULL) {
    return -1;
  }
  if (ccb_mkdir_p(dir) != CCB_OK) {
    free(dir);
    return -1;
  }
  char *tmp = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, dir);
    ccb_str_append(&s, "/.tmp-");
    ccb_str_appendf(&s, "%ld", (long)getpid());
    tmp = ccb_str_steal(&s);
  }
  if (ccb_mkdir_p(tmp) != CCB_OK) {
    free(tmp);
    free(dir);
    return -1;
  }
  int rc = -1;
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->d8);
  // Sized heap for the stdlib-only dex (see ccb_d8_heap_mb): the
  // input here is just the stdlib jar, so this lands on the floor.
  ccb_argv_pushf(&av, "-JXmx%ldm", ccb_d8_heap_mb(ccb_dex_file_bytes(stdlib)));
  ccb_argv_push(&av, "--min-api");
  ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
  ccb_argv_push(&av, "--lib");
  ccb_argv_push(&av, p->sdk->android_jar);
  ccb_argv_push(&av, "--output");
  ccb_argv_push(&av, tmp);
  ccb_argv_push(&av, stdlib);
  char **env = ccb_jvm_env(p, ar);
  ccb_proc_result r;
  // Deliberately NOT ccb_run_tool: a failed cache population must
  // not record a build diagnostic, it just falls back to the jar.
  if (ccb_proc_run(av.v, NULL, env, &r) == CCB_OK) {
    if (r.exit_code == 0) {
      char *dex = ccb_path_join(tmp, "classes.dex");
      char *dex2 = ccb_path_join(tmp, "classes2.dex");
      if (dex != NULL && ccb_path_exists(dex) && dex2 != NULL && !ccb_path_exists(dex2) &&
          ccb_dex_file_valid(dex) && rename(dex, dest) == 0) {
        rc = 0;
      }
      free(dex);
      free(dex2);
    }
    ccb_proc_result_free(&r);
  }
  free(av.v);
  ccb_remove_recursive(tmp);
  free(tmp);
  free(dir);
  return rc;
}

// The stdlib input for d8: the cached dex when one is valid (or can
// be made), else NULL to mean "use the jar". Never fails the build.
char *ccb_stdlib_dex_input(ccb_pipe *p, const char *stdlib) {
  char *key = ccb_stdlib_dex_key(p, stdlib);
  if (key == NULL) {
    return NULL;
  }
  char *dir = ccb_stdlib_dex_dir();
  if (dir == NULL) {
    free(key);
    return NULL;
  }
  char *name = NULL;
  {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, key);
    ccb_str_append(&s, ".dex");
    name = ccb_str_steal(&s);
  }
  char *path = ccb_path_join(dir, name);
  free(name);
  free(dir);
  if (path == NULL) {
    free(key);
    return NULL;
  }
  if (ccb_dex_file_valid(path)) {
    ccb_log_cache(&p->log, "dex", 1, "stdlib dex cache hit");
    free(key);
    return path;
  }
  if (!p->opts->dry_run && ccb_stdlib_dex_populate(p, stdlib, path) == 0) {
    ccb_log_info(&p->log, "stdlib dex cached: %s", path);
    free(key);
    return path;
  }
  free(path);
  free(key);
  return NULL;
}
