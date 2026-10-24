// ccb_config: validated project configuration from ccb.toml.
// Load reports EVERY problem at once into the diagnostic list, so one
// `ccb check` run shows the whole fix list. All stored paths are absolute.
#ifndef CCB_CONFIG_H
#define CCB_CONFIG_H

#include "ccb_common.h"
#include "ccb_diag.h"

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
  char *libs_dir;
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
  // [deps]
  char **jars;
  size_t jars_count;
  char **aars;
  size_t aars_count;
  // [deps] maven: raw "group:artifact:version" coordinates, validated
  // at load (exact versions only) but resolved to cache paths later:
  // builds and checks read the cache, `ccb setup` prefetches it.
  char **maven;
  size_t maven_count;
  int maven_central; // [deps] maven_central: also search Maven Central
  // [ndk]
  int has_ndk;
  char **ndk_abis;
  size_t ndk_abis_count;
  char **ndk_sources;
  size_t ndk_sources_count;
  char **ndk_cflags;
  size_t ndk_cflags_count;
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
} ccb_config;

// Search upward from start_dir for ccb.toml. When explicit is non-NULL it
// is used as-is (and must exist). Returns CCB_ERR_CONFIG when nothing is
// found; *out_path is a malloc'd absolute path on success.
ccb_status ccb_config_discover(const char *start_dir, const char *explicit, char **out_path);
// Parse + validate. On failure returns CCB_ERR_CONFIG, fills diags, and
// leaves *out NULL. On success *out owns the config.
ccb_status ccb_config_load(const char *path, const char *variant_override, ccb_config **out, ccb_diag_list *diags);
void ccb_config_free(ccb_config *c);

#endif
