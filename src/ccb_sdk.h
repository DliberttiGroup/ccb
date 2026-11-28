// ccb_sdk: resolve and validate the Android toolchain.
// Resolution order for the SDK home: ccb.toml [sdk] home, $ANDROID_HOME,
// $ANDROID_SDK_ROOT, ~/Android/Sdk. Kotlin home: ccb.toml [sdk]
// kotlin_home, $KOTLIN_HOME. Java: $JAVA_HOME/bin/java, else PATH.
// Every missing or wrong-version piece is reported at once.
#ifndef CCB_SDK_H
#define CCB_SDK_H

#include "ccb_common.h"
#include "ccb_diag.h"
#include "ccb_config.h"

typedef struct {
  char *sdk_home;
  char *build_tools_dir;
  char *build_tools_version;
  char *aapt2;
  // Where the effective aapt2/zipalign came from: "config", "env", or
  // "default". Owned strings, always set on a successful resolve.
  char *aapt2_source;
  char *d8;
  char *apksigner;
  char *zipalign;
  char *zipalign_source;
  char *r8_jar;
  // Where the effective r8 jar came from: "config" or "sdk-home"
  // (the setup-managed copy). NULL when neither exists; only
  // release+minify builds need r8, so that is not a resolve error.
  char *r8_source;
  char *android_jar;
  char *java;
  char *javac;
  int java_major;
  char *kotlin_home;
  char *kotlinc;
  char *kotlin_version;
  char *ndk_home;
  char *ndk_clang;
} ccb_sdk;

// The single toolchain identity (Step 7, audit D-3): one canonical
// string over the resolved tools, used as the global tool
// fingerprint (a mismatch empties the whole cache). Components are
// exactly the ones stage 0 recorded before: build-tools version,
// aapt2/zipalign paths, kotlinc version, java major, compile SDK.
// Deliberately NOT recorded here, and where they are keyed instead:
// NDK version and clang path — the native step keys the NDK version
// itself (resolve verifies the installed NDK matches it); cmake/make
// paths — not keyed anywhere (same accepted limitation as a
// same-version kotlinc/java swap, Step 2 decision); r8 — the dex
// step keys the r8 jar by content hash; kotlinc/java paths — only
// the versions are recorded, so a same-version swap stays a no-op.
// Owned; NULL fields read as "" (a successful resolve never leaves
// build-tools/kotlinc/java unset, but the identity must not crash
// on a partially-resolved SDK in tests).
char *ccb_sdk_toolchain_identity(const ccb_sdk *sdk,
  const ccb_config *cfg);
// 1 when the tool at path spawns: any exit code except 127 (the
// child's exec-failure exit) and no fatal signal. arg may be NULL
// for no arguments.
int ccb_sdk_tool_runnable(const char *path, const char *arg);
// On failure returns CCB_ERR_TOOLCHAIN, fills diags, leaves *out NULL.
ccb_status ccb_sdk_resolve(const ccb_config *cfg, ccb_sdk **out, ccb_diag_list *diags);
void ccb_sdk_free(ccb_sdk *sdk);
// Find an executable on PATH. Returns a malloc'd absolute path or NULL.
char *ccb_sdk_find_on_path(const char *name);
// Parse kotlinc -version stderr ("info: kotlinc-jvm 2.4.21 (JRE ...)").
// Returns a malloc'd version string ("2.4.21") or NULL when unparseable.
char *ccb_sdk_parse_kotlin_version(const char *text);
// Tool probe cache (build/intermediates/tool-probes.json): caches the
// parsed version of a tool binary so repeat builds skip re-spawning
// slow probes (kotlinc -version costs seconds of JVM startup). Keyed
// by tool path + mtime + size; any mismatch, missing entry, or corrupt
// file is a miss and the caller re-probes. Best-effort and never
// fatal: lookups only read, stores silently skip when the directory
// does not exist yet. build_dir NULL means "no cache" for both.
char *ccb_sdk_probe_cache_lookup(const char *build_dir, const char *tool, const char *path);
void ccb_sdk_probe_cache_store(const char *build_dir, const char *tool, const char *path, const char *version);
// Resolve a usable java exactly the way ccb_sdk_resolve does:
// $JAVA_HOME/bin/java, else java on PATH, with major version >= 17.
// On success *out holds a malloc'd path and *major_out the major
// version; on failure returns CCB_ERR_TOOLCHAIN with *out NULL.
ccb_status ccb_sdk_find_java(char **out, int *major_out);

#endif
