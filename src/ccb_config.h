// ccb_config: validated project configuration from ccb.toml.
// Load reports EVERY problem at once into the diagnostic list, so one
// `ccb check` run shows the whole fix list. All stored paths are absolute.
#ifndef CCB_CONFIG_H
#define CCB_CONFIG_H

#include "ccb_common.h"
#include "ccb_diag.h"

// One preview scenario: declared runtime facts for a project (see
// docs/preview.md). A scenario never overrides a static literal: the
// merge only fills unknowns and picks branches. All strings are
// config-owned (malloc'd).
typedef struct {
  char *screen; // screen the branch pick applies to
  char *entry; // when/if entry label, e.g. "is Loaded", "else"
} ccb_scenario_branch;
typedef struct {
  char *label; // list label; rows replace placeholder rows only when
               // the list has no static children
  char **rows;
  size_t nrows;
} ccb_scenario_list;
typedef struct {
  char *label; // textfield label; text is shown as "[label: text]"
  char *text;
} ccb_scenario_field;
typedef struct {
  char *label; // button label; values fill unknown arguments
               // left-to-right, never a static literal
  char **values;
  size_t nvalues;
} ccb_scenario_args;
typedef struct {
  char *name; // single segment: no dots (dots separate the sections)
  ccb_scenario_branch *branches;
  size_t nbranches;
  ccb_scenario_list *lists;
  size_t nlists;
  ccb_scenario_field *fields;
  size_t nfields;
  ccb_scenario_args *args;
  size_t nargs;
} ccb_scenario;

// A library module: [modules.<name>] in ccb.toml. After load the
// modules array is in topological order (dependencies first). The
// same struct also holds one [deps.library.<name>] path library: then
// dir is the library project root (or the built .aar file when
// is_file), version is the library project's [project] version, and
// depends is always empty.
typedef struct {
  char *name;
  char *dir; // absolute module root
  char **depends;
  size_t depends_count;
  // Standard-layout paths under dir (NULL when absent on disk).
  char *manifest; // <dir>/src/main/AndroidManifest.xml (required)
  char *res; // <dir>/src/main/res
  char *assets; // <dir>/src/main/assets
  char *kotlin_dir; // <dir>/src/main/kotlin
  char *java_dir; // <dir>/src/main/java
  char *package; // from the module manifest (required)
  int is_file; // dir is a built .aar file, not a project root
  char *version; // library project version (path libraries only)
} ccb_module;

typedef struct {
  // Meta.
  char *config_path;
  char *project_root;
  long ccb_version;
  // [project]
  char *name;
  char *package;
  char *version;
  long version_code;
  long min_sdk;
  long target_sdk;
  long compile_sdk;
  // [paths] (NULL when not declared)
  char *manifest;
  char *res;
  char *assets;
  char *kotlin_dir;
  char *java_dir;
  // Extra Java source roots (e.g. SDLActivity from a vendored SDL2
  // tree): [paths] java_extra = [...]. Collected alongside java_dir.
  char **java_extra_dirs;
  size_t java_extra_dirs_count;
  // [sdk]
  char *sdk_home;
  char *build_tools;
  char *ndk_version;
  char *kotlin_home;
  char *r8_jar;
  // [sdk] tool overrides, stored exactly as written (a relative value
  // is cwd-relative at invocation time, never rebased). NULL when unset.
  char *aapt2_override;
  char *zipalign_override;
  char *adb_override;
  // [signing.debug]
  char *debug_store;
  char *debug_alias;
  // [signing.release]
  int has_release_signing;
  char *release_store;
  char *release_alias;
  char *release_store_pw_env;
  char *release_key_pw_env;
  // [build]
  char *variant;
  int minify;
  int shrink_resources;
  char *jvm_target;
  char *proguard_rules;
  char *build_dir;
  // [build] kotlinc_daemon: opt-in Kotlin compiler daemon (default
  // off: no background process). kotlinc_daemon_idle_seconds: idle
  // seconds before the daemon shuts itself down (default 300).
  int kotlinc_daemon;
  long kotlinc_daemon_idle_seconds;
  // [build] kotlin_plugins: exact "group:artifact:version" coordinates
  // of kotlinc compiler-plugin jars, resolved via Maven (setup
  // prefetches, builds read the cache) into ccb_resolved by the
  // pipeline's stage 0 and passed to kotlinc as -Xplugin=<jar>.
  // kotlin_plugin_options: "plugin-id:key=value" strings, passed as
  // -P plugin:<id>:<key>=<value>. Plugin coordinates must be exact
  // versions: a compiler plugin has to match the kotlinc version, so
  // ranges and snapshots are rejected.
  char **kotlin_plugins;
  size_t kotlin_plugins_count;
  char **kotlin_plugin_options;
  size_t kotlin_plugin_options_count;
  // [deps]
  char **jars;
  size_t jars_count;
  char **aars;
  size_t aars_count;
  // [deps] maven: raw "group:artifact:version" coordinates, validated
  // at load (exact versions, version ranges, LATEST, RELEASE, and
  // -SNAPSHOT are all accepted) but resolved to cache paths later:
  // builds and checks read the cache, `ccb setup` prefetches it.
  char **maven;
  size_t maven_count;
  int maven_central; // [deps] maven_central: also search Maven Central
  // [deps] maven_repos: extra repositories, searched after Google
  // Maven (and Maven Central when enabled), in declared order. Each
  // entry is "url" or "url username password-source", where the
  // password source is "env:VAR" (read from the environment at
  // resolve time) or a literal password. Credentials never appear in
  // logs or diagnostics.
  char **maven_repos;
  size_t maven_repos_count;
  // [deps] maven_profiles: profile ids activated explicitly (like
  // mvn -P). A profile with <activeByDefault> is active only when
  // this list is empty.
  char **maven_profiles;
  size_t maven_profiles_count;
  // [deps] maven_exclude: "group:artifact" (every version) or
  // "group:artifact:version" (that version only). An excluded artifact
  // never resolves, even transitively; excluding a direct dependency
  // is a config error at load.
  char **maven_exclude;
  size_t maven_exclude_count;
  // [ndk]
  int has_ndk;
  char **ndk_abis;
  size_t ndk_abis_count;
  char **ndk_sources;
  size_t ndk_sources_count;
  char **ndk_cflags;
  size_t ndk_cflags_count;
  // [cmake] external native build: drive an existing CMakeLists.txt
  // instead of CCB's own clang-per-source step. The built .so files
  // are copied into intermediates/jni/<abi>/ and packaged exactly
  // like [ndk]-built libs. [ndk] abis must still be declared so
  // packaging knows which ABIs to include.
  int has_cmake;
  char *cmake_lists;
  char **cmake_args;
  size_t cmake_args_count;
  // [preview] (parsed now, used by M5)
  int has_preview;
  char *preview_styles;
  char *preview_flow;
  char *preview_resources; // [preview] resources: dir of strings.xml/colors.xml
  char *preview_layout; // [preview] layout: dir (or file) of layout XML
  char *preview_nav_graph; // [preview] nav_graph: nav-graph XML file or dir
  long preview_phone_width;
  long preview_phone_height;
  int preview_strict;
  int preview_gate; // [preview] gate: opt-in preview gate for check/build
  // [preview.bindings]: parallel arrays mapping code identifiers to
  // component labels for Slice 1 handler attribution. NULL/empty
  // when the table is absent.
  char **preview_binding_keys;
  char **preview_binding_values;
  size_t preview_binding_count;
  // [preview.scenario.<name>] tables, in sub-table spelling (the TOML
  // subset has no inline tables, so the plan's `branches = {...}`
  // example is written as [preview.scenario.<name>.branches] with one
  // key per entry): branches maps screen -> when/if entry label,
  // lists maps list label -> row strings, fields maps textfield
  // label -> text, args maps button label -> values that fill
  // unknown arguments. Config-owned (malloc'd); freed by
  // ccb_config_free. The preview merge copies what it needs into
  // the project arena, so a scenario never outlives its config.
  ccb_scenario *preview_scenarios;
  size_t preview_scenario_count;
  // [modules.<name>] library modules, in topological order
  // (dependencies first). Empty when the project is single-module.
  ccb_module *modules;
  size_t modules_count;
  // [library]: present when this project IS a library (it builds an
  // .aar, not an APK). [deps.library.<name>] path libraries this app
  // consumes, in declaration order (built before modules).
  int is_library;
  ccb_module *libraries;
  size_t libraries_count;
} ccb_config;

// Search upward from start_dir for ccb.toml. When explicit is non-NULL it
// is used as-is (and must exist). Returns CCB_ERR_CONFIG when nothing is
// found; *out_path is a malloc'd absolute path on success.
ccb_status ccb_config_discover(const char *start_dir, const char *explicit, char **out_path);
// Parse + validate. On failure returns CCB_ERR_CONFIG, fills diags, and
// leaves *out NULL. On success *out owns the config.
ccb_status ccb_config_load(const char *path, const char *variant_override, ccb_config **out, ccb_diag_list *diags);
void ccb_config_free(ccb_config *c);
// Find a scenario by name in a loaded config, or NULL when absent.
// Borrowed: the config owns it.
const ccb_scenario *ccb_config_scenario(const ccb_config *c,
  const char *name);

#endif
