// ccb_config implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_toml.h"
#include "ccb_config.h"
#include <unistd.h>

ccb_status ccb_config_discover(const char *start_dir, const char *explicit, char **out_path) {
  if (explicit != NULL) {
    if (!ccb_path_exists(explicit)) {
      return CCB_ERR_CONFIG;
    }
    if (explicit[0] == '/') {
      *out_path = ccb_xstrdup(explicit);
    } else {
      char cwd[4096];
      if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return CCB_ERR_CONFIG;
      }
      *out_path = ccb_path_join(cwd, explicit);
    }
    return CCB_OK;
  }
  char *dir = ccb_xstrdup(start_dir);
  while (1) {
    char *candidate = ccb_path_join(dir, "ccb.toml");
    int found = ccb_path_exists(candidate) && !ccb_path_is_dir(candidate);
    if (found) {
      *out_path = candidate;
      free(dir);
      return CCB_OK;
    }
    free(candidate);
    char *parent = ccb_dirname(dir);
    if (strcmp(parent, dir) == 0) {
      free(parent);
      break;
    }
    free(dir);
    dir = parent;
  }
  free(dir);
  return CCB_ERR_CONFIG;
}

typedef struct {
  ccb_diag_list *diags;
  const ccb_toml_doc *doc;
} ccb_validator;

static void ccb_vdiag(ccb_validator *v, const char *code, const char *msg) {
  ccb_diag d = ccb_diag_make(code, "config", msg);
  ccb_diag_list_push(v->diags, &d);
}

static void ccb_vdiagf(ccb_validator *v, const char *code, const char *fmt, const char *a, const char *b) {
  // Two-string formatting covers every validation message below.
  ccb_str s;
  ccb_str_init(&s);
  const char *p = fmt;
  int which = 0;
  while (*p != '\0') {
    if (p[0] == '%' && p[1] == 's') {
      ccb_str_append(&s, which == 0 ? a : b);
      which++;
      p += 2;
    } else {
      ccb_str_append_c(&s, *p);
      p++;
    }
  }
  ccb_diag d = ccb_diag_make(code, "config", s.buf);
  ccb_diag_list_push(v->diags, &d);
  ccb_str_free(&s);
}

// Check every key in a table against the allowed set. Unknown keys are
// hard errors listing the key and the table.
static void ccb_check_unknown_keys(ccb_validator *v, const char *table_name, const char *const *allowed, size_t allowed_count) {
  const ccb_toml_table *t = ccb_toml_find_table(v->doc, table_name);
  if (t == NULL) {
    return;
  }
  for (size_t i = 0; i < t->count; i++) {
    int ok = 0;
    for (size_t j = 0; j < allowed_count; j++) {
      if (strcmp(t->kvs[i].key, allowed[j]) == 0) {
        ok = 1;
        break;
      }
    }
    if (!ok) {
      ccb_vdiagf(v, "CCB-E1001", "unknown key '%s' in table [%s]", t->kvs[i].key, table_name);
    }
  }
}

// Typed field readers. Each records a diagnostic on type mismatch and
// returns 0; on success returns 1 and fills *out.
static int ccb_get_str(ccb_validator *v, const char *table_name, const char *key, char **out) {
  const ccb_toml_table *t = ccb_toml_find_table(v->doc, table_name);
  const ccb_toml_value *val = ccb_toml_find_value(t, key);
  if (val == NULL) {
    return 0;
  }
  if (val->type != CCB_TOML_STRING) {
    ccb_vdiagf(v, "CCB-E1007", "key '%s' must be a string", key, "");
    return 0;
  }
  *out = ccb_xstrdup(val->str);
  return 1;
}

static int ccb_get_int(ccb_validator *v, const char *table_name, const char *key, long *out) {
  const ccb_toml_table *t = ccb_toml_find_table(v->doc, table_name);
  const ccb_toml_value *val = ccb_toml_find_value(t, key);
  if (val == NULL) {
    return 0;
  }
  if (val->type != CCB_TOML_INT) {
    ccb_vdiagf(v, "CCB-E1007", "key '%s' must be an integer", key, "");
    return 0;
  }
  *out = val->integer;
  return 1;
}

static int ccb_get_bool(ccb_validator *v, const char *table_name, const char *key, int *out) {
  const ccb_toml_table *t = ccb_toml_find_table(v->doc, table_name);
  const ccb_toml_value *val = ccb_toml_find_value(t, key);
  if (val == NULL) {
    return 0;
  }
  if (val->type != CCB_TOML_BOOL) {
    ccb_vdiagf(v, "CCB-E1007", "key '%s' must be a boolean", key, "");
    return 0;
  }
  *out = val->boolean;
  return 1;
}

static int ccb_get_str_array(ccb_validator *v, const char *table_name, const char *key, char ***out, size_t *count) {
  const ccb_toml_table *t = ccb_toml_find_table(v->doc, table_name);
  const ccb_toml_value *val = ccb_toml_find_value(t, key);
  if (val == NULL) {
    return 0;
  }
  if (val->type != CCB_TOML_STR_ARRAY) {
    ccb_vdiagf(v, "CCB-E1007", "key '%s' must be an array of strings", key, "");
    return 0;
  }
  *count = val->count;
  *out = ccb_xmalloc((val->count == 0 ? 1 : val->count) * sizeof(**out));
  for (size_t i = 0; i < val->count; i++) {
    (*out)[i] = ccb_xstrdup(val->items[i]);
  }
  return 1;
}

static char *ccb_resolve(const char *root, const char *value) {
  if (value[0] == '/') {
    return ccb_xstrdup(value);
  }
  return ccb_path_join(root, value);
}

static int ccb_key_present(const ccb_toml_doc *doc, const char *table_name, const char *key) {
  const ccb_toml_table *t = ccb_toml_find_table(doc, table_name);
  return ccb_toml_find_value(t, key) != NULL;
}

static void ccb_require_file(ccb_validator *v, const char *path, const char *what) {
  if (!ccb_path_exists(path) || ccb_path_is_dir(path)) {
    ccb_vdiagf(v, "CCB-E1006", "%s does not exist: %s", what, path);
  }
}

static void ccb_require_dir(ccb_validator *v, const char *path, const char *what) {
  if (!ccb_path_is_dir(path)) {
    ccb_vdiagf(v, "CCB-E1006", "%s does not exist: %s", what, path);
  }
}

// A [preview] layout / nav_graph value may be a file or a directory.
static void ccb_require_path(ccb_validator *v, const char *path,
    const char *what) {
  if (!ccb_path_exists(path)) {
    ccb_vdiagf(v, "CCB-E1006", "%s does not exist: %s", what, path);
  }
}

void ccb_config_free(ccb_config *c) {
  if (c == NULL) {
    return;
  }
  free(c->config_path);
  free(c->project_root);
  free(c->name);
  free(c->package);
  free(c->version);
  free(c->manifest);
  free(c->res);
  free(c->assets);
  free(c->kotlin_dir);
  free(c->java_dir);
  free(c->libs_dir);
  free(c->sdk_home);
  free(c->build_tools);
  free(c->ndk_version);
  free(c->kotlin_home);
  free(c->r8_jar);
  free(c->aapt2_override);
  free(c->zipalign_override);
  free(c->debug_store);
  free(c->debug_alias);
  free(c->release_store);
  free(c->release_alias);
  free(c->release_store_pw_env);
  free(c->release_key_pw_env);
  free(c->variant);
  free(c->jvm_target);
  free(c->proguard_rules);
  free(c->build_dir);
  free(c->preview_styles);
  free(c->preview_flow);
  free(c->preview_resources);
  free(c->preview_layout);
  free(c->preview_nav_graph);
  for (size_t i = 0; i < c->preview_binding_count; i++) {
    free(c->preview_binding_keys[i]);
    free(c->preview_binding_values[i]);
  }
  free(c->preview_binding_keys);
  free(c->preview_binding_values);
  ccb_free_str_array(c->jars, c->jars_count);
  ccb_free_str_array(c->aars, c->aars_count);
  ccb_free_str_array(c->ndk_abis, c->ndk_abis_count);
  ccb_free_str_array(c->ndk_sources, c->ndk_sources_count);
  ccb_free_str_array(c->ndk_cflags, c->ndk_cflags_count);
  free(c);
}

ccb_status ccb_config_load(const char *path, const char *variant_override, ccb_config **out, ccb_diag_list *diags) {
  *out = NULL;
  size_t diag_before = diags->count;
  char *text = NULL;
  size_t text_len = 0;
  if (ccb_read_file(path, &text, &text_len) != CCB_OK) {
    ccb_diag d = ccb_diag_make("CCB-E1009", "config", "cannot read config file");
    d.file = ccb_xstrdup(path);
    ccb_diag_list_push(diags, &d);
    return CCB_ERR_CONFIG;
  }
  ccb_toml_error *perrors = NULL;
  size_t nperrors = 0;
  ccb_toml_doc *doc = ccb_toml_parse(text, &perrors, &nperrors);
  free(text);
  if (doc == NULL) {
    for (size_t i = 0; i < nperrors; i++) {
      ccb_diag d = ccb_diag_make("CCB-E1008", "config", perrors[i].message);
      d.file = ccb_xstrdup(path);
      d.line = perrors[i].line;
      ccb_diag_list_push(diags, &d);
    }
    ccb_toml_free_errors(perrors, nperrors);
    return CCB_ERR_CONFIG;
  }

  ccb_validator v;
  v.diags = diags;
  v.doc = doc;
  ccb_config *c = ccb_xcalloc(1, sizeof(*c));
  c->config_path = ccb_xstrdup(path);
  c->project_root = ccb_dirname(path);
  c->version_code = 1;
  c->build_tools = ccb_xstrdup("35.0.0");
  c->debug_alias = ccb_xstrdup("androiddebugkey");
  c->variant = ccb_xstrdup("debug");
  c->jvm_target = ccb_xstrdup("17");
  c->build_dir = ccb_path_join(c->project_root, "build");
  c->preview_phone_width = 390;
  c->preview_phone_height = 844;

  // Unknown keys, per table.
  {
    static const char *root_keys[] = { "ccb_version" };
    static const char *project_keys[] = { "name", "package", "version", "version_code", "min_sdk", "target_sdk", "compile_sdk" };
    static const char *paths_keys[] = { "manifest", "res", "assets", "kotlin", "java", "libs" };
    static const char *sdk_keys[] = { "home", "build_tools", "ndk_version", "kotlin_home", "r8_jar", "aapt2", "zipalign" };
    static const char *debug_keys[] = { "store", "alias" };
    static const char *release_keys[] = { "store", "alias", "store_password_env", "key_password_env" };
    static const char *build_keys[] = { "variant", "minify", "shrink_resources", "jvm_target", "proguard_rules" };
    static const char *deps_keys[] = { "jars", "aars" };
    static const char *ndk_keys[] = { "abis", "c_sources", "cflags" };
    static const char *preview_keys[] = { "styles", "flow", "phone_width", "phone_height", "strict", "gate", "resources", "layout", "nav_graph" };
    ccb_check_unknown_keys(&v, "", root_keys, 1);
    ccb_check_unknown_keys(&v, "project", project_keys, 7);
    ccb_check_unknown_keys(&v, "paths", paths_keys, 6);
    ccb_check_unknown_keys(&v, "sdk", sdk_keys, 7);
    ccb_check_unknown_keys(&v, "signing.debug", debug_keys, 2);
    ccb_check_unknown_keys(&v, "signing.release", release_keys, 4);
    ccb_check_unknown_keys(&v, "build", build_keys, 5);
    ccb_check_unknown_keys(&v, "deps", deps_keys, 2);
    ccb_check_unknown_keys(&v, "ndk", ndk_keys, 3);
    ccb_check_unknown_keys(&v, "preview", preview_keys, 9);
  }

  // Root: ccb_version.
  {
    long ver = 0;
    if (!ccb_get_int(&v, "", "ccb_version", &ver)) {
      const ccb_toml_table *root = ccb_toml_find_table(doc, "");
      if (ccb_toml_find_value(root, "ccb_version") == NULL) {
        ccb_vdiag(&v, "CCB-E1002", "missing required key 'ccb_version' (must be 1)");
      }
    } else if (ver != 1) {
      ccb_vdiag(&v, "CCB-E1003", "unsupported ccb_version: only version 1 is supported");
    } else {
      c->ccb_version = ver;
    }
  }

  // [project]
  if (!ccb_get_str(&v, "project", "name", &c->name) || c->name[0] == '\0') {
    if (c->name == NULL && !ccb_key_present(doc, "project", "name")) {
      ccb_vdiag(&v, "CCB-E1004", "missing required key 'project.name'");
    } else if (c->name != NULL) {
      ccb_vdiag(&v, "CCB-E1005", "project.name must not be empty");
    }
  }
  if (!ccb_get_str(&v, "project", "package", &c->package) || c->package[0] == '\0') {
    if (c->package == NULL && !ccb_key_present(doc, "project", "package")) {
      ccb_vdiag(&v, "CCB-E1004", "missing required key 'project.package'");
    } else if (c->package != NULL) {
      ccb_vdiag(&v, "CCB-E1005", "project.package must not be empty");
    }
  }
  if (!ccb_get_str(&v, "project", "version", &c->version) || c->version[0] == '\0') {
    if (c->version == NULL && !ccb_key_present(doc, "project", "version")) {
      ccb_vdiag(&v, "CCB-E1004", "missing required key 'project.version'");
    } else if (c->version != NULL) {
      ccb_vdiag(&v, "CCB-E1005", "project.version must not be empty");
    }
  }
  {
    long vc = 0;
    if (ccb_get_int(&v, "project", "version_code", &vc)) {
      if (vc < 0) {
        ccb_vdiag(&v, "CCB-E1005", "project.version_code must be >= 0");
      } else {
        c->version_code = vc;
      }
    }
  }
  int have_min = ccb_get_int(&v, "project", "min_sdk", &c->min_sdk);
  int have_target = ccb_get_int(&v, "project", "target_sdk", &c->target_sdk);
  int have_compile = ccb_get_int(&v, "project", "compile_sdk", &c->compile_sdk);
  if (!have_min && !ccb_key_present(doc, "project", "min_sdk")) {
    ccb_vdiag(&v, "CCB-E1004", "missing required key 'project.min_sdk'");
  } else if (have_min && (c->min_sdk < 21 || c->min_sdk > 35)) {
    ccb_vdiag(&v, "CCB-E1005", "project.min_sdk must be in 21..35");
  }
  if (!have_target && !ccb_key_present(doc, "project", "target_sdk")) {
    ccb_vdiag(&v, "CCB-E1004", "missing required key 'project.target_sdk'");
  }
  if (!have_compile && !ccb_key_present(doc, "project", "compile_sdk")) {
    ccb_vdiag(&v, "CCB-E1004", "missing required key 'project.compile_sdk'");
  } else if (have_compile && (c->compile_sdk < 34 || c->compile_sdk > 35)) {
    ccb_vdiag(&v, "CCB-E1005", "project.compile_sdk must be 34 or 35");
  }
  if (have_min && have_target && c->min_sdk > c->target_sdk) {
    ccb_vdiag(&v, "CCB-E1005", "project.min_sdk must not exceed project.target_sdk");
  }
  if (have_target && have_compile && c->target_sdk > c->compile_sdk) {
    ccb_vdiag(&v, "CCB-E1005", "project.target_sdk must not exceed project.compile_sdk");
  }

  // [paths]
  {
    char *m = NULL;
    if (!ccb_get_str(&v, "paths", "manifest", &m)) {
      if (!ccb_key_present(doc, "paths", "manifest")) {
        ccb_vdiag(&v, "CCB-E1004", "missing required key 'paths.manifest'");
      }
    } else {
      c->manifest = ccb_resolve(c->project_root, m);
      free(m);
    }
    char *p = NULL;
    if (ccb_get_str(&v, "paths", "res", &p)) {
      c->res = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "paths", "assets", &p)) {
      c->assets = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "paths", "kotlin", &p)) {
      c->kotlin_dir = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "paths", "java", &p)) {
      c->java_dir = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "paths", "libs", &p)) {
      c->libs_dir = ccb_resolve(c->project_root, p);
      free(p);
    }
  }

  // [sdk]
  {
    char *p = NULL;
    if (ccb_get_str(&v, "sdk", "home", &p)) {
      c->sdk_home = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "sdk", "build_tools", &p)) {
      free(c->build_tools);
      c->build_tools = p;
    }
    p = NULL;
    if (ccb_get_str(&v, "sdk", "ndk_version", &p)) {
      c->ndk_version = p;
    }
    p = NULL;
    if (ccb_get_str(&v, "sdk", "kotlin_home", &p)) {
      c->kotlin_home = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "sdk", "r8_jar", &p)) {
      c->r8_jar = ccb_resolve(c->project_root, p);
      free(p);
    }
    // Tool overrides are stored exactly as written: a relative value
    // stays relative and is interpreted against the caller's working
    // directory when the tool runs, not against the project root or
    // the SDK home. No existence check here: ccb_sdk_resolve validates
    // the effective path (config, env, or default) at Stage 0.
    p = NULL;
    if (ccb_get_str(&v, "sdk", "aapt2", &p)) {
      c->aapt2_override = p;
    }
    p = NULL;
    if (ccb_get_str(&v, "sdk", "zipalign", &p)) {
      c->zipalign_override = p;
    }
  }

  // [signing.debug]
  {
    char *p = NULL;
    if (ccb_get_str(&v, "signing.debug", "store", &p)) {
      c->debug_store = ccb_expand_home(p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "signing.debug", "alias", &p)) {
      free(c->debug_alias);
      c->debug_alias = p;
    }
  }

  // [signing.release]
  if (ccb_toml_find_table(doc, "signing.release") != NULL) {
    c->has_release_signing = 1;
    char *p = NULL;
    if (!ccb_get_str(&v, "signing.release", "store", &p)) {
      if (!ccb_key_present(doc, "signing.release", "store")) {
        ccb_vdiag(&v, "CCB-E1004", "missing required key 'signing.release.store'");
      }
    } else {
      c->release_store = ccb_expand_home(p);
      free(p);
    }
    p = NULL;
    if (!ccb_get_str(&v, "signing.release", "alias", &p)) {
      if (!ccb_key_present(doc, "signing.release", "alias")) {
        ccb_vdiag(&v, "CCB-E1004", "missing required key 'signing.release.alias'");
      }
    } else {
      c->release_alias = p;
    }
    p = NULL;
    if (!ccb_get_str(&v, "signing.release", "store_password_env", &p)) {
      if (!ccb_key_present(doc, "signing.release", "store_password_env")) {
        ccb_vdiag(&v, "CCB-E1004", "missing required key 'signing.release.store_password_env'");
      }
    } else {
      c->release_store_pw_env = p;
    }
    p = NULL;
    if (!ccb_get_str(&v, "signing.release", "key_password_env", &p)) {
      if (!ccb_key_present(doc, "signing.release", "key_password_env")) {
        ccb_vdiag(&v, "CCB-E1004", "missing required key 'signing.release.key_password_env'");
      }
    } else {
      c->release_key_pw_env = p;
    }
  }

  // [build]
  {
    char *p = NULL;
    if (ccb_get_str(&v, "build", "variant", &p)) {
      free(c->variant);
      c->variant = p;
    }
    if (strcmp(c->variant, "debug") != 0 && strcmp(c->variant, "release") != 0) {
      ccb_vdiag(&v, "CCB-E1005", "build.variant must be 'debug' or 'release'");
    }
    // The CLI --variant override applies before validation that depends
    // on the variant (e.g. minify requires release), so a project with
    // minify = true builds with --variant release even when the TOML
    // default variant is debug.
    if (variant_override != NULL) {
      if (strcmp(variant_override, "debug") != 0 &&
          strcmp(variant_override, "release") != 0) {
        ccb_vdiag(&v, "CCB-E1005", "--variant must be 'debug' or 'release'");
      } else {
        free(c->variant);
        c->variant = ccb_xstrdup(variant_override);
      }
    }
    int b = 0;
    if (ccb_get_bool(&v, "build", "minify", &b)) {
      c->minify = b;
    }
    b = 0;
    if (ccb_get_bool(&v, "build", "shrink_resources", &b)) {
      c->shrink_resources = b;
    }
    if (c->minify && strcmp(c->variant, "debug") == 0) {
      ccb_vdiag(&v, "CCB-E1005", "build.minify requires variant = 'release'");
    }
    p = NULL;
    if (ccb_get_str(&v, "build", "jvm_target", &p)) {
      free(c->jvm_target);
      c->jvm_target = p;
    }
    if (strcmp(c->jvm_target, "11") != 0 && strcmp(c->jvm_target, "17") != 0) {
      ccb_vdiag(&v, "CCB-E1005", "build.jvm_target must be '11' or '17'");
    }
    p = NULL;
    if (ccb_get_str(&v, "build", "proguard_rules", &p)) {
      c->proguard_rules = ccb_resolve(c->project_root, p);
      free(p);
    }
    if (c->minify && c->proguard_rules == NULL) {
      ccb_vdiag(&v, "CCB-E1004", "build.minify requires 'build.proguard_rules'");
    }
  }

  // [deps]
  ccb_get_str_array(&v, "deps", "jars", &c->jars, &c->jars_count);
  ccb_get_str_array(&v, "deps", "aars", &c->aars, &c->aars_count);
  for (size_t i = 0; i < c->jars_count; i++) {
    char *resolved = ccb_resolve(c->project_root, c->jars[i]);
    free(c->jars[i]);
    c->jars[i] = resolved;
  }
  for (size_t i = 0; i < c->aars_count; i++) {
    char *resolved = ccb_resolve(c->project_root, c->aars[i]);
    free(c->aars[i]);
    c->aars[i] = resolved;
  }

  // [ndk]
  if (ccb_toml_find_table(doc, "ndk") != NULL) {
    c->has_ndk = 1;
    ccb_get_str_array(&v, "ndk", "abis", &c->ndk_abis, &c->ndk_abis_count);
    ccb_get_str_array(&v, "ndk", "c_sources", &c->ndk_sources, &c->ndk_sources_count);
    ccb_get_str_array(&v, "ndk", "cflags", &c->ndk_cflags, &c->ndk_cflags_count);
    if (c->ndk_abis_count == 0) {
      if (!ccb_key_present(doc, "ndk", "abis")) {
        ccb_vdiag(&v, "CCB-E1004", "missing required key 'ndk.abis'");
      } else {
        ccb_vdiag(&v, "CCB-E1005", "ndk.abis must not be empty");
      }
    }
    static const char *known_abis[] = { "arm64-v8a", "armeabi-v7a", "x86_64", "x86" };
    for (size_t i = 0; i < c->ndk_abis_count; i++) {
      int ok = 0;
      for (size_t j = 0; j < 4; j++) {
        if (strcmp(c->ndk_abis[i], known_abis[j]) == 0) {
          ok = 1;
          break;
        }
      }
      if (!ok) {
        ccb_vdiagf(&v, "CCB-E1005", "unknown ABI '%s' in ndk.abis", c->ndk_abis[i], "");
      }
    }
    for (size_t i = 0; i < c->ndk_sources_count; i++) {
      char *resolved = ccb_resolve(c->project_root, c->ndk_sources[i]);
      free(c->ndk_sources[i]);
      c->ndk_sources[i] = resolved;
    }
  }

  // [preview]
  if (ccb_toml_find_table(doc, "preview") != NULL) {
    c->has_preview = 1;
    char *p = NULL;
    if (ccb_get_str(&v, "preview", "styles", &p)) {
      c->preview_styles = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "preview", "flow", &p)) {
      c->preview_flow = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "preview", "resources", &p)) {
      c->preview_resources = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "preview", "layout", &p)) {
      c->preview_layout = ccb_resolve(c->project_root, p);
      free(p);
    }
    p = NULL;
    if (ccb_get_str(&v, "preview", "nav_graph", &p)) {
      c->preview_nav_graph = ccb_resolve(c->project_root, p);
      free(p);
    }
    long w = 0;
    if (ccb_get_int(&v, "preview", "phone_width", &w)) {
      c->preview_phone_width = w;
    }
    long h = 0;
    if (ccb_get_int(&v, "preview", "phone_height", &h)) {
      c->preview_phone_height = h;
    }
    int strict = 0;
    if (ccb_get_bool(&v, "preview", "strict", &strict)) {
      c->preview_strict = strict;
    }
    int gate = 0;
    if (ccb_get_bool(&v, "preview", "gate", &gate)) {
      c->preview_gate = gate;
    }
    // [preview.bindings]: code identifier -> component label, both
    // strings. A non-string value is the existing wrong-type error
    // (CCB-E1007); the table itself needs no unknown-key check
    // because any identifier may be a key.
    {
      const ccb_toml_table *bt = ccb_toml_find_table(doc,
        "preview.bindings");
      if (bt != NULL) {
        for (size_t i = 0; i < bt->count; i++) {
          if (bt->kvs[i].value.type != CCB_TOML_STRING) {
            ccb_vdiagf(&v, "CCB-E1007",
              "key '%s' must be a string", bt->kvs[i].key, "");
            continue;
          }
          size_t n = c->preview_binding_count;
          c->preview_binding_keys = ccb_xrealloc(
            c->preview_binding_keys, (n + 1) * sizeof(*c->preview_binding_keys));
          c->preview_binding_values = ccb_xrealloc(
            c->preview_binding_values,
            (n + 1) * sizeof(*c->preview_binding_values));
          c->preview_binding_keys[n] = ccb_xstrdup(bt->kvs[i].key);
          c->preview_binding_values[n] =
            ccb_xstrdup(bt->kvs[i].value.str);
          c->preview_binding_count = n + 1;
        }
      }
    }
  }

  // Path existence checks (only when no earlier error already explains
  // the path, and only for declared paths).
  if (c->manifest != NULL) {
    ccb_require_file(&v, c->manifest, "manifest");
  }
  if (c->res != NULL) {
    ccb_require_dir(&v, c->res, "res directory");
  }
  if (c->assets != NULL) {
    ccb_require_dir(&v, c->assets, "assets directory");
  }
  if (c->kotlin_dir != NULL) {
    ccb_require_dir(&v, c->kotlin_dir, "kotlin directory");
  }
  if (c->java_dir != NULL) {
    ccb_require_dir(&v, c->java_dir, "java directory");
  }
  if (c->libs_dir != NULL) {
    ccb_require_dir(&v, c->libs_dir, "libs directory");
  }
  if (c->sdk_home != NULL) {
    ccb_require_dir(&v, c->sdk_home, "sdk home");
  }
  if (c->kotlin_home != NULL) {
    ccb_require_dir(&v, c->kotlin_home, "kotlin home");
  }
  if (c->r8_jar != NULL) {
    ccb_require_file(&v, c->r8_jar, "r8 jar");
  }
  if (c->proguard_rules != NULL) {
    ccb_require_file(&v, c->proguard_rules, "proguard rules");
  }
  if (c->debug_store != NULL) {
    ccb_require_file(&v, c->debug_store, "debug keystore");
  }
  if (c->release_store != NULL) {
    ccb_require_file(&v, c->release_store, "release keystore");
  }
  for (size_t i = 0; i < c->jars_count; i++) {
    ccb_require_file(&v, c->jars[i], "jar dependency");
  }
  for (size_t i = 0; i < c->aars_count; i++) {
    ccb_require_file(&v, c->aars[i], "aar dependency");
  }
  for (size_t i = 0; i < c->ndk_sources_count; i++) {
    ccb_require_file(&v, c->ndk_sources[i], "ndk source");
  }
  if (c->preview_styles != NULL) {
    ccb_require_dir(&v, c->preview_styles, "preview styles directory");
  }
  if (c->preview_flow != NULL) {
    ccb_require_dir(&v, c->preview_flow, "preview flow directory");
  }
  if (c->preview_resources != NULL) {
    ccb_require_dir(&v, c->preview_resources, "preview resources directory");
  }
  if (c->preview_layout != NULL) {
    ccb_require_path(&v, c->preview_layout, "preview layout path");
  }
  if (c->preview_nav_graph != NULL) {
    ccb_require_path(&v, c->preview_nav_graph, "preview nav_graph path");
  }

  ccb_toml_free_doc(doc);
  if (diags->count > diag_before) {
    ccb_config_free(c);
    return CCB_ERR_CONFIG;
  }
  *out = c;
  return CCB_OK;
}
