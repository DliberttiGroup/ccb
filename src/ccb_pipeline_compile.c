// ccb_pipeline_compile: shared compile providers for the app steps
// and the module builder: aapt2 compile/link argv, the compile
// classpath, kotlinc, and javac.
//
// Both paths used to build these command lines inline: the same
// flags, assembled twice, so a fix to one never reached the other
// (audit D1). The providers below build the argv once; app and
// module callers differ only in the file lists and extras they pass
// (upstream R.java files, R.txt output, version/overlay flags),
// which stay with the caller because they are semantic differences,
// not duplication.

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

// ---------- aapt2 compile ----------

// aapt2 compile argv: `aapt2 compile --dir <res> -o <zip>`.
void ccb_aapt2_compile_argv(ccb_pipe *p, ccb_argv *av,
    const char *res_dir, const char *out_zip) {
  ccb_argv_push(av, p->sdk->aapt2);
  ccb_argv_push(av, "compile");
  ccb_argv_push(av, "--dir");
  ccb_argv_push(av, res_dir);
  ccb_argv_push(av, "-o");
  ccb_argv_push(av, out_zip);
}

// Run one aapt2 compile: the shared step behind app compile-res and
// module resource compile. The suggestion names the owner, so the
// diagnostic still says which side failed.
ccb_status ccb_aapt2_compile(ccb_pipe *p, const char *res_dir,
    const char *out_zip, const char *suggestion) {
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_aapt2_compile_argv(p, &av, res_dir, out_zip);
  ccb_status rc = ccb_run_tool(p, "aapt2-compile", "CCB-E3002", av.v,
    NULL, suggestion);
  free(av.v);
  return rc;
}

// ---------- aapt2 link ----------

// aapt2 link argv shared by the app link and the module link:
// `aapt2 link [--proto-format] -o <out> --manifest <m> -I <jar>
// --java <gen> [--output-text-symbols <rtxt>] --auto-add-overlay
// --min-sdk-version <n> --target-sdk-version <n>`.
// rtxt is NULL for the app (only modules write R.txt); is_aab adds
// --proto-format for the app AAB link. Callers append their own
// extras after this prefix, in order: app version, --extra-packages,
// -R overlays; module the compiled.zip input.
void ccb_aapt2_link_argv(ccb_pipe *p, ccb_argv *av, const char *out,
    const char *manifest, const char *gen_dir, const char *rtxt,
    int is_aab) {
  ccb_argv_push(av, p->sdk->aapt2);
  ccb_argv_push(av, "link");
  if (is_aab) {
    ccb_argv_push(av, "--proto-format");
  }
  ccb_argv_push(av, "-o");
  ccb_argv_push(av, out);
  ccb_argv_push(av, "--manifest");
  ccb_argv_push(av, manifest);
  ccb_argv_push(av, "-I");
  ccb_argv_push(av, p->sdk->android_jar);
  ccb_argv_push(av, "--java");
  ccb_argv_push(av, gen_dir);
  if (rtxt != NULL) {
    ccb_argv_push(av, "--output-text-symbols");
    ccb_argv_push(av, rtxt);
  }
  ccb_argv_push(av, "--auto-add-overlay");
  ccb_argv_push(av, "--min-sdk-version");
  ccb_argv_pushf(av, "%ld", p->cfg->min_sdk);
  ccb_argv_push(av, "--target-sdk-version");
  ccb_argv_pushf(av, "%ld", p->cfg->target_sdk);
}

// ---------- compile classpath ----------

// Compile classpath, identical for the app and a module:
// android.jar, then [deps] jars, then AAR classes.jars (declared
// AARs and already-built modules, in registration order). Owned.
char *ccb_build_classpath(ccb_pipe *p) {
  ccb_str cp;
  ccb_str_init(&cp);
  ccb_str_append(&cp, p->sdk->android_jar);
  for (size_t i = 0; i < p->resolved.jars_count; i++) {
    ccb_str_append_c(&cp, ':');
    ccb_str_append(&cp, p->resolved.jars[i]);
  }
  for (size_t i = 0; i < p->aar_class_jars_count; i++) {
    ccb_str_append_c(&cp, ':');
    ccb_str_append(&cp, p->aar_class_jars[i]);
  }
  return ccb_str_steal(&cp);
}

// ---------- kotlinc ----------

// kotlinc argv shared by the app compile-code step and the module
// compile step: `kotlinc -jvm-target <t> -d <out> -cp <cp>` +
// compiler-plugin args + sources in order: Kotlin, generated R.java,
// extra (a module's upstream dependency R.java files; empty for the
// app), Java. kotlinc resolves Java symbols from the sources passed
// on the command line, so every list is on the argv.
void ccb_kotlinc_argv(ccb_pipe *p, ccb_argv *av, const char *out_dir,
    const char *cp, char **kt, size_t nkt, char **rjava, size_t nrjava,
    char **extra, size_t nextra, char **java, size_t njava) {
  ccb_argv_push(av, p->sdk->kotlinc);
  ccb_argv_push(av, "-jvm-target");
  ccb_argv_push(av, p->cfg->jvm_target);
  ccb_argv_push(av, "-d");
  ccb_argv_push(av, out_dir);
  ccb_argv_push(av, "-cp");
  ccb_argv_push(av, cp);
  ccb_push_plugin_args(p, av);
  for (size_t i = 0; i < nkt; i++) {
    ccb_argv_push(av, kt[i]);
  }
  for (size_t i = 0; i < nrjava; i++) {
    ccb_argv_push(av, rjava[i]);
  }
  for (size_t i = 0; i < nextra; i++) {
    ccb_argv_push(av, extra[i]);
  }
  for (size_t i = 0; i < njava; i++) {
    ccb_argv_push(av, java[i]);
  }
}

// Run kotlinc over the source lists (via the daemon when enabled,
// else directly — ccb_run_kotlinc owns that choice, so both paths
// share the daemon behavior too). env is the JVM env from
// ccb_jvm_env, borrowed.
ccb_status ccb_run_kotlinc_sources(ccb_pipe *p, ccb_arena *ar,
    char **env, const char *out_dir, const char *cp, char **kt,
    size_t nkt, char **rjava, size_t nrjava, char **extra,
    size_t nextra, char **java, size_t njava,
    const char *suggestion) {
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_kotlinc_argv(p, &av, out_dir, cp, kt, nkt, rjava, nrjava, extra,
    nextra, java, njava);
  ccb_status rc = ccb_run_kotlinc(p, av.v, env, suggestion);
  free(av.v);
  return rc;
}

// ---------- javac ----------

// javac argv shared by the app compile-code step and the module
// compile step: `javac -encoding UTF-8 -source <t> -target <t>
// -cp <out>:<cp> -d <out>` + sources. The two file lists are pushed
// in order: the app passes (generated R.java, Java sources) —
// kotlinc emits no R.class files, so javac must compile R.java —
// while a module passes (Java sources, none): its R.class is
// stripped from classes.jar anyway, and kotlinc already resolved
// its R symbols from the R.java sources. The difference is semantic,
// so it stays in the callers' lists, not in this builder.
void ccb_javac_argv(ccb_pipe *p, ccb_argv *av, const char *out_dir,
    const char *cp, char **first, size_t nfirst, char **second,
    size_t nsecond) {
  // -cp is <out>:<cp> so javac resolves the classes kotlinc just
  // wrote. -encoding UTF-8 regardless of the host locale: Java files
  // are written in UTF-8 (a non-ASCII comment must not fail the
  // build on a non-UTF-8 machine).
  ccb_str jcp;
  ccb_str_init(&jcp);
  ccb_str_append(&jcp, out_dir);
  ccb_str_append_c(&jcp, ':');
  ccb_str_append(&jcp, cp);
  ccb_argv_push(av, p->sdk->javac);
  ccb_argv_push(av, "-encoding");
  ccb_argv_push(av, "UTF-8");
  ccb_argv_push(av, "-source");
  ccb_argv_push(av, p->cfg->jvm_target);
  ccb_argv_push(av, "-target");
  ccb_argv_push(av, p->cfg->jvm_target);
  ccb_argv_push(av, "-cp");
  ccb_argv_push(av, jcp.buf);
  ccb_str_free(&jcp);
  ccb_argv_push(av, "-d");
  ccb_argv_push(av, out_dir);
  for (size_t i = 0; i < nfirst; i++) {
    ccb_argv_push(av, first[i]);
  }
  for (size_t i = 0; i < nsecond; i++) {
    ccb_argv_push(av, second[i]);
  }
}

// Run javac over the source lists. env is the JVM env from
// ccb_jvm_env, borrowed.
ccb_status ccb_run_javac_sources(ccb_pipe *p, ccb_arena *ar,
    char **env, const char *out_dir, const char *cp, char **first,
    size_t nfirst, char **second, size_t nsecond,
    const char *suggestion) {
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_javac_argv(p, &av, out_dir, cp, first, nfirst, second, nsecond);
  ccb_status rc = ccb_run_tool(p, "javac", "CCB-E3005", av.v, env,
    suggestion);
  free(av.v);
  return rc;
}
