// ccb_pipeline_cmake.c: the [cmake] external native build — drive an
// existing CMakeLists.txt instead of CCB's own clang-per-source step.
// A project brings its own CMakeLists.txt (with the NDK toolchain
// via the standard android.toolchain.cmake); CCB configures and
// builds it per ABI and copies the resulting .so files into
// intermediates/jni/<abi>/, where the package step picks them up
// exactly like [ndk]-built libs. No host-tool preamble, no
// project-specific generated-file steps: those belong in the
// project's own CMakeLists.txt.
#include "ccb_pipeline_internal.h"

// cmake: the SDK-bundled 3.22.1 first, then PATH.
static char *ccb_cmake_find(ccb_pipe *p) {
  char *in_sdk = ccb_path_join3(p->sdk->sdk_home, "cmake/3.22.1/bin", "cmake");
  if (access(in_sdk, X_OK) == 0) {
    return in_sdk;
  }
  free(in_sdk);
  return ccb_sdk_find_on_path("cmake");
}

// 16 KB page size for arm64-v8a (Android 15+ 16 KB-page devices
// refuse .so files linked with the 4 KB default): the same flags the
// [ndk] clang link step uses, as CMake linker flags.
static void ccb_cmake_page_flags(ccb_argv *av, const char *abi) {
  if (strcmp(abi, "arm64-v8a") != 0) {
    return;
  }
  ccb_argv_push(av,
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-z,max-page-size=16384");
  ccb_argv_push(av,
    "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384");
  ccb_argv_push(av,
    "-DCMAKE_MODULE_LINKER_FLAGS=-Wl,-z,max-page-size=16384");
}

// Push one "-Dname=value" argument built from a prefix and a value.
static void ccb_cmake_define(ccb_argv *av, const char *name,
    const char *value) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "-D");
  ccb_str_append(&s, name);
  ccb_str_append_c(&s, '=');
  ccb_str_append(&s, value);
  ccb_argv_push(av, s.buf);
  ccb_str_free(&s);
}

ccb_status ccb_step_cmake(ccb_pipe *p, const char *abi) {
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  char *lists_dir = ccb_dirname(p->cfg->cmake_lists);
  char *build_dir = ccb_path_join3(p->intermediates, "cmake", abi);
  char *abi_dir = ccb_path_join3(p->intermediates, "jni", abi);
  char *toolchain = ccb_path_join3(p->sdk->ndk_home,
    "build/cmake", "android.toolchain.cmake");
  char *cmake = ccb_cmake_find(p);
  char *make = ccb_sdk_find_on_path("make");
  ccb_status rc = CCB_OK;
  if (cmake == NULL) {
    ccb_pipe_diag(p, "CCB-E3012", "cmake",
      "cmake not found: install SDK cmake 3.22.1 or put cmake on PATH");
    rc = CCB_ERR_BUILD;
    goto cmake_done;
  }
  if (make == NULL) {
    ccb_pipe_diag(p, "CCB-E3012", "cmake",
      "make not found: the Unix Makefiles generator needs make on PATH");
    rc = CCB_ERR_BUILD;
    goto cmake_done;
  }
  if (!p->opts->dry_run) {
    // Wipe the ABI output dir first: a renamed library in the
    // project's CMakeLists must not leave a stale .so behind to be
    // packaged (same rule as the [ndk] clang step).
    ccb_remove_recursive(abi_dir);
    if (ccb_mkdir_p(build_dir) != CCB_OK ||
        ccb_mkdir_p(abi_dir) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3016", "cmake",
        "cannot create cmake output directory");
      rc = CCB_ERR_BUILD;
      goto cmake_done;
    }
  }
  {
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, cmake);
    ccb_argv_push(&av, "-S");
    ccb_argv_push(&av, lists_dir);
    ccb_argv_push(&av, "-B");
    ccb_argv_push(&av, build_dir);
    ccb_argv_push(&av, "-G");
    ccb_argv_push(&av, "Unix Makefiles");
    ccb_cmake_define(&av, "CMAKE_TOOLCHAIN_FILE", toolchain);
    ccb_cmake_define(&av, "ANDROID_ABI", abi);
    {
      char plat[32];
      snprintf(plat, sizeof plat, "android-%ld", p->cfg->min_sdk);
      ccb_cmake_define(&av, "ANDROID_PLATFORM", plat);
    }
    ccb_cmake_define(&av, "CMAKE_BUILD_TYPE",
      strcmp(p->cfg->variant, "release") == 0 ? "Release" : "Debug");
    ccb_cmake_define(&av, "ANDROID_NDK", p->sdk->ndk_home);
    ccb_cmake_page_flags(&av, abi);
    for (size_t i = 0; i < p->cfg->cmake_args_count; i++) {
      ccb_argv_push(&av, p->cfg->cmake_args[i]);
    }
    rc = ccb_run_tool(p, "cmake", "CCB-E3012", av.v, NULL,
      "check the CMakeLists.txt for errors");
    free(av.v);
    if (rc != CCB_OK) {
      goto cmake_done;
    }
  }
  {
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, cmake);
    ccb_argv_push(&av, "--build");
    ccb_argv_push(&av, build_dir);
    ccb_argv_push(&av, "--parallel");
    ccb_argv_push(&av, "2");
    rc = ccb_run_tool(p, "cmake", "CCB-E3013", av.v, NULL,
      "check the native sources for errors");
    free(av.v);
    if (rc != CCB_OK) {
      goto cmake_done;
    }
  }
  // Copy the built shared libraries into intermediates/jni/<abi>/.
  // Skipped in dry-run: the build above only printed its commands,
  // so there is nothing to copy yet.
  if (!p->opts->dry_run) {
    char **sos = NULL;
    size_t nsos = 0;
    if (ccb_collect_files(build_dir, ".so", &sos, &nsos) != CCB_OK) {
      ccb_pipe_diag(p, "CCB-E3013", "cmake",
        "cannot read cmake build output");
      rc = CCB_ERR_BUILD;
      goto cmake_done;
    }
    if (nsos == 0) {
      ccb_pipe_diag(p, "CCB-E3013", "cmake",
        "cmake built no shared libraries");
      rc = CCB_ERR_BUILD;
      goto cmake_done;
    }
    for (size_t i = 0; i < nsos && rc == CCB_OK; i++) {
      char *base = ccb_basename(sos[i]);
      char *dst = ccb_path_join(abi_dir, base);
      char *data = NULL;
      size_t len = 0;
      if (ccb_read_file(sos[i], &data, &len) != CCB_OK ||
          ccb_write_file(dst, data, len) != CCB_OK) {
        ccb_pipe_diag(p, "CCB-E3013", "cmake",
          "cannot copy built shared library into intermediates");
        rc = CCB_ERR_BUILD;
      }
      free(base);
      free(dst);
      free(data);
    }
    ccb_free_str_array(sos, nsos);
  }
cmake_done:
  free(lists_dir);
  free(build_dir);
  free(abi_dir);
  free(toolchain);
  free(cmake);
  free(make);
  return rc;
}
