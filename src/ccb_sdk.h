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

// On failure returns CCB_ERR_TOOLCHAIN, fills diags, leaves *out NULL.
ccb_status ccb_sdk_resolve(const ccb_config *cfg, ccb_sdk **out, ccb_diag_list *diags);
void ccb_sdk_free(ccb_sdk *sdk);
// Find an executable on PATH. Returns a malloc'd absolute path or NULL.
char *ccb_sdk_find_on_path(const char *name);
// Resolve a usable java exactly the way ccb_sdk_resolve does:
// $JAVA_HOME/bin/java, else java on PATH, with major version >= 17.
// On success *out holds a malloc'd path and *major_out the major
// version; on failure returns CCB_ERR_TOOLCHAIN with *out NULL.
ccb_status ccb_sdk_find_java(char **out, int *major_out);

#endif
