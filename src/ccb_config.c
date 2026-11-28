// ccb_config implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_toml.h"
#include "ccb_config.h"
#include "ccb_daemon.h"
#include "ccb_manifest.h"
#include "ccb_zip.h"
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

// Version-spec check for [deps] maven entries (kept here rather
// than in ccb_maven so config does not depend on the resolver module,
// which itself depends on config). Exactly three non-empty
// colon-separated parts, no path-unsafe characters, and a version
// that is either an exact version, a Maven version range
// ("[1.0,2.0)", "[1.0]", "[1.0,)", "(,2.0]"), LATEST, RELEASE, or a
// -SNAPSHOT version. A range never becomes a path component: the
// resolver turns it into a concrete version before any fetch.
static int ccb_config_maven_version_spec_ok(const char *s, size_t n) {
  if (n == 0) {
    return 0;
  }
  if (s[0] == '[' || s[0] == '(') {
    // Range: first char '[' or '(', last char ']' or ')', balanced
    // brackets, only range characters inside, at least one digit.
    if (n < 3 || (s[n - 1] != ']' && s[n - 1] != ')')) {
      return 0;
    }
    int depth = 0;
    int digit = 0;
    for (size_t k = 0; k < n; k++) {
      char ch = s[k];
      int ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-' || ch == '_' ||
        ch == '+' || ch == ',' || ch == ' ' || ch == '[' || ch == ']' ||
        ch == '(' || ch == ')';
      if (!ok) {
        return 0;
      }
      if (ch >= '0' && ch <= '9') {
        digit = 1;
      }
      if (ch == '[' || ch == '(') {
        depth++;
      }
      if (ch == ']' || ch == ')') {
        depth--;
      }
      if (depth < 0) {
        return 0;
      }
    }
    return depth == 0 && digit;
  }
  if ((n == 6 && memcmp(s, "LATEST", 6) == 0) ||
      (n == 7 && memcmp(s, "RELEASE", 7) == 0)) {
    return 1;
  }
  if (n >= 9 && memcmp(s + n - 9, "-SNAPSHOT", 9) == 0) {
    return 1;
  }
  for (size_t k = 0; k < n; k++) {
    char ch = s[k];
    if (ch == '[' || ch == ']' || ch == '(' || ch == ')') {
      return 0;
    }
  }
  return 1;
}

static int ccb_config_maven_coord_ok(const char *s) {
  const char *c1 = strchr(s, ':');
  const char *c2 = c1 != NULL ? strchr(c1 + 1, ':') : NULL;
  if (c1 == NULL || c2 == NULL || strchr(c2 + 1, ':') != NULL) {
    return 0;
  }
  const char *parts[3];
  size_t lens[3];
  parts[0] = s;
  lens[0] = (size_t)(c1 - s);
  parts[1] = c1 + 1;
  lens[1] = (size_t)(c2 - (c1 + 1));
  parts[2] = c2 + 1;
  lens[2] = strlen(c2 + 1);
  for (int i = 0; i < 3; i++) {
    if (lens[i] == 0) {
      return 0;
    }
    for (size_t k = 0; k < lens[i]; k++) {
      char ch = parts[i][k];
      if (ch <= ' ' || ch == '/' || ch == '\\' || ch == ':' || ch == 0x7f) {
        return 0;
      }
    }
  }
  if (lens[2] == 1 && parts[2][0] == '.') {
    return 0;
  }
  if (lens[2] == 2 && parts[2][0] == '.' && parts[2][1] == '.') {
    return 0;
  }
  return ccb_config_maven_version_spec_ok(parts[2], lens[2]);
}

// Exact-version coordinate check for [build] kotlin_plugins: same
// shape as a maven coordinate, but the version must be exact. A
// compiler plugin has to match the kotlinc version, so ranges,
// LATEST, RELEASE, and -SNAPSHOT are rejected here.
static int ccb_config_plugin_coord_ok(const char *s) {
  const char *c1 = strchr(s, ':');
  const char *c2 = c1 != NULL ? strchr(c1 + 1, ':') : NULL;
  if (c1 == NULL || c2 == NULL || strchr(c2 + 1, ':') != NULL) {
    return 0;
  }
  const char *v = c2 + 1;
  size_t n = strlen(v);
  if (n == 0) {
    return 0;
  }
  if (v[0] == '[' || v[0] == '(') {
    return 0;
  }
  if ((n == 6 && memcmp(v, "LATEST", 6) == 0) ||
      (n == 7 && memcmp(v, "RELEASE", 7) == 0)) {
    return 0;
  }
  if (n >= 9 && memcmp(v + n - 9, "-SNAPSHOT", 9) == 0) {
    return 0;
  }
  return ccb_config_maven_coord_ok(s);
}

// One [deps] maven_repos entry: "url" or
// "url username password-source". The password source is "env:NAME"
// (read from the environment at resolve time) or a literal password.
// Fields are space-separated; a URL never contains a space.
static int ccb_config_maven_repo_ok(const char *s) {
  const char *sp1 = strchr(s, ' ');
  size_t url_len = sp1 != NULL ? (size_t)(sp1 - s) : strlen(s);
  if (url_len == 0) {
    return 0;
  }
  if (url_len < 7 || (memcmp(s, "http://", 7) != 0 &&
      memcmp(s, "https://", 8) != 0 && memcmp(s, "file://", 7) != 0)) {
    return 0;
  }
  // No userinfo in the URL: credentials belong in the username and
  // password-source fields, never embedded where they could be
  // logged as part of a URL.
  for (size_t k = 0; k < url_len; k++) {
    if (s[k] == '@') {
      return 0;
    }
  }
  if (sp1 == NULL) {
    return 1;
  }
  const char *user = sp1 + 1;
  const char *sp2 = strchr(user, ' ');
  if (sp2 == NULL) {
    return 0;
  }
  size_t user_len = (size_t)(sp2 - user);
  if (user_len == 0) {
    return 0;
  }
  for (size_t k = 0; k < user_len; k++) {
    if (user[k] == ':') {
      return 0;
    }
  }
  const char *pw = sp2 + 1;
  if (pw[0] == '\0' || strchr(pw, ' ') != NULL) {
    return 0;
  }
  if (memcmp(pw, "env:", 4) == 0) {
    // Environment variable name: [A-Za-z_][A-Za-z0-9_]*.
    const char *name = pw + 4;
    if (name[0] == '\0') {
      return 0;
    }
    for (size_t k = 0; name[k] != '\0'; k++) {
      char ch = name[k];
      int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        ch == '_' || (k > 0 && ch >= '0' && ch <= '9');
      if (!ok) {
        return 0;
      }
    }
  }
  return 1;
}

// One [build] kotlin_plugin_options entry: "plugin-id:key=value".
// The id and key are dotted identifiers; the value is any non-empty
// string without spaces.
static int ccb_config_plugin_option_ok(const char *s) {
  const char *colon = strchr(s, ':');
  if (colon == NULL || colon == s) {
    return 0;
  }
  for (const char *p = s; p < colon; p++) {
    char ch = *p;
    int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
    if (!ok) {
      return 0;
    }
  }
  const char *eq = strchr(colon + 1, '=');
  if (eq == NULL || eq == colon + 1 || eq[1] == '\0') {
    return 0;
  }
  for (const char *p = colon + 1; p < eq; p++) {
    char ch = *p;
    int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
    if (!ok) {
      return 0;
    }
  }
  for (const char *p = eq + 1; *p != '\0'; p++) {
    if (*p <= ' ') {
      return 0;
    }
  }
  return 1;
}

// One field of a maven_exclude entry: non-empty, no path-unsafe
// characters, never "." or ".." (same rules as coordinate fields).
static int ccb_config_maven_excl_part_ok(const char *s, size_t n) {
  if (n == 0) {
    return 0;
  }
  for (size_t k = 0; k < n; k++) {
    char ch = s[k];
    if (ch <= ' ' || ch == '/' || ch == '\\' || ch == ':' || ch == 0x7f) {
      return 0;
    }
  }
  if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) {
    return 0;
  }
  return 1;
}

// Validate a maven_exclude entry: "group:artifact" or
// "group:artifact:version", every field passing the part rules.
static int ccb_config_maven_exclude_ok(const char *s) {
  const char *c1 = strchr(s, ':');
  const char *c2 = c1 != NULL ? strchr(c1 + 1, ':') : NULL;
  if (c1 == NULL) {
    return 0;
  }
  if (c2 != NULL && strchr(c2 + 1, ':') != NULL) {
    return 0;
  }
  if (!ccb_config_maven_excl_part_ok(s, (size_t)(c1 - s)) ||
      !ccb_config_maven_excl_part_ok(c1 + 1, c2 != NULL ?
        (size_t)(c2 - (c1 + 1)) : strlen(c1 + 1))) {
    return 0;
  }
  if (c2 != NULL && !ccb_config_maven_excl_part_ok(c2 + 1, strlen(c2 + 1))) {
    return 0;
  }
  return 1;
}

// Split a "group:artifact" or "group:artifact:version" string into
// its group and artifact fields (no allocation; views into s).
// Returns 0 when there is no ':' or either field is empty.
static int ccb_config_maven_ga(const char *s, const char **g, size_t *glen,
    const char **a, size_t *alen) {
  const char *c1 = strchr(s, ':');
  if (c1 == NULL) {
    return 0;
  }
  const char *c2 = strchr(c1 + 1, ':');
  *g = s;
  *glen = (size_t)(c1 - s);
  *a = c1 + 1;
  *alen = c2 != NULL ? (size_t)(c2 - (c1 + 1)) : strlen(c1 + 1);
  return *glen > 0 && *alen > 0;
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
  ccb_free_str_array(c->java_extra_dirs, c->java_extra_dirs_count);
  free(c->sdk_home);
  free(c->build_tools);
  free(c->ndk_version);
  free(c->kotlin_home);
  free(c->r8_jar);
  free(c->aapt2_override);
  free(c->zipalign_override);
  free(c->adb_override);
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
  ccb_free_str_array(c->maven, c->maven_count);
  ccb_free_str_array(c->maven_repos, c->maven_repos_count);
  ccb_free_str_array(c->maven_profiles, c->maven_profiles_count);
  ccb_free_str_array(c->maven_exclude, c->maven_exclude_count);
  ccb_free_str_array(c->kotlin_plugins, c->kotlin_plugins_count);
  ccb_free_str_array(c->kotlin_plugin_options,
    c->kotlin_plugin_options_count);
  ccb_free_str_array(c->ndk_abis, c->ndk_abis_count);
  ccb_free_str_array(c->ndk_sources, c->ndk_sources_count);
  ccb_free_str_array(c->ndk_cflags, c->ndk_cflags_count);
  free(c->cmake_lists);
  ccb_free_str_array(c->cmake_args, c->cmake_args_count);
  for (size_t i = 0; i < c->modules_count; i++) {
    ccb_module *m = &c->modules[i];
    free(m->name);
    free(m->dir);
    ccb_free_str_array(m->depends, m->depends_count);
    free(m->manifest);
    free(m->res);
    free(m->assets);
    free(m->kotlin_dir);
    free(m->java_dir);
    free(m->package);
    free(m->version);
  }
  free(c->modules);
  for (size_t i = 0; i < c->libraries_count; i++) {
    ccb_module *m = &c->libraries[i];
    free(m->name);
    free(m->dir);
    ccb_free_str_array(m->depends, m->depends_count);
    free(m->manifest);
    free(m->res);
    free(m->assets);
    free(m->kotlin_dir);
    free(m->java_dir);
    free(m->package);
    free(m->version);
  }
  free(c->libraries);
  for (size_t i = 0; i < c->preview_scenario_count; i++) {
    ccb_scenario *sc = &c->preview_scenarios[i];
    size_t j = 0;
    free(sc->name);
    for (j = 0; j < sc->nbranches; j++) {
      free(sc->branches[j].screen);
      free(sc->branches[j].entry);
    }
    free(sc->branches);
    for (j = 0; j < sc->nlists; j++) {
      free(sc->lists[j].label);
      ccb_free_str_array(sc->lists[j].rows, sc->lists[j].nrows);
    }
    free(sc->lists);
    for (j = 0; j < sc->nfields; j++) {
      free(sc->fields[j].label);
      free(sc->fields[j].text);
    }
    free(sc->fields);
    for (j = 0; j < sc->nargs; j++) {
      free(sc->args[j].label);
      ccb_free_str_array(sc->args[j].values, sc->args[j].nvalues);
    }
    free(sc->args);
  }
  free(c->preview_scenarios);
  free(c);
}

const ccb_scenario *ccb_config_scenario(const ccb_config *c,
    const char *name) {
  size_t i = 0;
  if (c == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0; i < c->preview_scenario_count; i++) {
    if (strcmp(c->preview_scenarios[i].name, name) == 0) {
      return &c->preview_scenarios[i];
    }
  }
  return NULL;
}

// ---------- library modules ----------

// Find a parsed module by name, or NULL.
static ccb_module *ccb_find_module(ccb_config *c, const char *name) {
  for (size_t i = 0; i < c->modules_count; i++) {
    if (strcmp(c->modules[i].name, name) == 0) {
      return &c->modules[i];
    }
  }
  return NULL;
}

// Validate the module graph and reorder modules topologically
// (dependencies first). An unknown dependency is CCB-E1011; a cycle is
// CCB-E1010 naming the cycle path. Returns 1 when the graph is usable;
// on failure the config still fails to load, so the order is left
// as-is.
static int ccb_sort_modules(ccb_validator *v, ccb_config *c) {
  int ok = 1;
  for (size_t i = 0; i < c->modules_count; i++) {
    for (size_t j = 0; j < c->modules[i].depends_count; j++) {
      if (ccb_find_module(c, c->modules[i].depends[j]) == NULL) {
        ccb_vdiagf(v, "CCB-E1011", "module '%s' depends on unknown module '%s'",
          c->modules[i].name, c->modules[i].depends[j]);
        ok = 0;
      }
    }
  }
  if (!ok) {
    return 0;
  }
  // Kahn-style placement: repeatedly place a module whose dependencies
  // are all placed. Quadratic in the module count, which is tiny.
  char *placed = ccb_xcalloc(c->modules_count == 0 ? 1 : c->modules_count, 1);
  ccb_module *sorted = ccb_xmalloc(
    (c->modules_count == 0 ? 1 : c->modules_count) * sizeof(*sorted));
  size_t nsorted = 0;
  while (nsorted < c->modules_count) {
    int progress = 0;
    for (size_t i = 0; i < c->modules_count; i++) {
      if (placed[i]) {
        continue;
      }
      int ready = 1;
      for (size_t j = 0; j < c->modules[i].depends_count; j++) {
        ccb_module *d = ccb_find_module(c, c->modules[i].depends[j]);
        size_t di = (size_t)(d - c->modules);
        if (!placed[di]) {
          ready = 0;
          break;
        }
      }
      if (ready) {
        sorted[nsorted++] = c->modules[i];
        placed[i] = 1;
        progress = 1;
      }
    }
    if (!progress) {
      break;
    }
  }
  if (nsorted < c->modules_count) {
    // Stuck: the unplaced modules contain a cycle. Walk from the first
    // unplaced module through unplaced dependencies until a module
    // repeats; that repeat closes the cycle path.
    size_t start = 0;
    while (placed[start]) {
      start++;
    }
    size_t *path = ccb_xmalloc((c->modules_count + 1) * sizeof(*path));
    size_t npath = 0;
    size_t cur = start;
    size_t close_at = 0;
    int found = 0;
    while (!found) {
      path[npath++] = cur;
      size_t next = c->modules_count;
      for (size_t j = 0; j < c->modules[cur].depends_count; j++) {
        ccb_module *d = ccb_find_module(c, c->modules[cur].depends[j]);
        size_t di = (size_t)(d - c->modules);
        if (!placed[di]) {
          next = di;
          break;
        }
      }
      // An unplaced module always has an unplaced dependency, else it
      // would have been placed; the walk must repeat within the set.
      // The guard keeps a future edit from turning this into an
      // out-of-bounds read: without a next step, report the path.
      if (next == c->modules_count) {
        close_at = 0;
        found = 1;
        break;
      }
      for (size_t k = 0; k < npath; k++) {
        if (path[k] == next) {
          close_at = k;
          found = 1;
          break;
        }
      }
      cur = next;
    }
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "module dependency cycle: ");
    for (size_t k = close_at; k < npath; k++) {
      if (k > close_at) {
        ccb_str_append(&msg, " -> ");
      }
      ccb_str_append(&msg, c->modules[path[k]].name);
    }
    ccb_str_append(&msg, " -> ");
    ccb_str_append(&msg, c->modules[path[close_at]].name);
    ccb_vdiag(v, "CCB-E1010", msg.buf);
    ccb_str_free(&msg);
    free(path);
    free(sorted);
    free(placed);
    return 0;
  }
  if (c->modules_count > 0) {
    memcpy(c->modules, sorted, c->modules_count * sizeof(*sorted));
  }
  free(sorted);
  free(placed);
  return 1;
}

// ---------- section loaders ----------
// Each loader fills one part of ccb_config from the parsed TOML doc
// and appends diagnostics for what it finds. Validation is
// accumulate-and-report: a loader never aborts the load, so every
// section is checked and all errors are reported at once, in section
// order. No loader changes a rule, a default, or a message — the
// code below is the old ccb_config_load body, moved verbatim into
// one function per section.

// Optional string field: when present, store it as written; a wrong
// type is already CCB-E1007 from the getter.
static void ccb_load_plain_str(ccb_validator *v,
    const char *table, const char *key, char **field) {
  char *p = NULL;
  if (ccb_get_str(v, table, key, &p)) {
    *field = p;
  }
}

// Optional string field, resolved against the project root.
static void ccb_load_resolved_str(ccb_validator *v,
    const char *root, const char *table, const char *key, char **field) {
  char *p = NULL;
  if (ccb_get_str(v, table, key, &p)) {
    *field = ccb_resolve(root, p);
    free(p);
  }
}

// Optional string field replacing a default: the old value is freed.
static void ccb_load_replace_str(ccb_validator *v,
    const char *table, const char *key, char **field) {
  char *p = NULL;
  if (ccb_get_str(v, table, key, &p)) {
    free(*field);
    *field = p;
  }
}

// Optional string field with ~ expansion (keystore paths).
static void ccb_load_home_str(ccb_validator *v,
    const char *table, const char *key, char **field) {
  char *p = NULL;
  if (ccb_get_str(v, table, key, &p)) {
    *field = ccb_expand_home(p);
    free(p);
  }
}

// Required string field: missing is CCB-E1004; when present it is
// stored as written, or ~ expanded when expand_home is set.
static void ccb_load_required_str(ccb_validator *v, const ccb_toml_doc *doc,
    const char *table, const char *key, char **field, int expand_home) {
  char *p = NULL;
  if (!ccb_get_str(v, table, key, &p)) {
    if (!ccb_key_present(doc, table, key)) {
      ccb_vdiagf(v, "CCB-E1004", "missing required key '%s.%s'", table, key);
    }
    return;
  }
  if (expand_home) {
    *field = ccb_expand_home(p);
    free(p);
  } else {
    *field = p;
  }
}

// Unknown keys, per table.
static void ccb_load_unknown_keys(ccb_validator *v) {
  static const char *root_keys[] = { "ccb_version" };
  static const char *project_keys[] = { "name", "package", "version", "version_code", "min_sdk", "target_sdk", "compile_sdk" };
  static const char *paths_keys[] = { "manifest", "res", "assets", "kotlin", "java", "java_extra" };
  static const char *sdk_keys[] = { "home", "build_tools", "ndk_version", "kotlin_home", "r8_jar", "aapt2", "zipalign", "adb" };
  static const char *debug_keys[] = { "store", "alias" };
  static const char *release_keys[] = { "store", "alias", "store_password_env", "key_password_env" };
  static const char *build_keys[] = { "variant", "minify", "shrink_resources", "jvm_target", "proguard_rules", "kotlinc_daemon", "kotlinc_daemon_idle_seconds", "kotlin_plugins", "kotlin_plugin_options" };
  static const char *deps_keys[] = { "jars", "aars", "maven", "maven_central", "maven_exclude", "maven_repos", "maven_profiles" };
  static const char *ndk_keys[] = { "abis", "c_sources", "cflags" };
  static const char *cmake_keys[] = { "lists", "args" };
  static const char *preview_keys[] = { "styles", "flow", "phone_width", "phone_height", "strict", "gate", "resources", "layout", "nav_graph" };
  ccb_check_unknown_keys(v, "", root_keys, 1);
  ccb_check_unknown_keys(v, "project", project_keys, 7);
  ccb_check_unknown_keys(v, "paths", paths_keys, 6);
  ccb_check_unknown_keys(v, "sdk", sdk_keys, 7);
  ccb_check_unknown_keys(v, "signing.debug", debug_keys, 2);
  ccb_check_unknown_keys(v, "signing.release", release_keys, 4);
  ccb_check_unknown_keys(v, "build", build_keys, 9);
  ccb_check_unknown_keys(v, "deps", deps_keys, 7);
  ccb_check_unknown_keys(v, "ndk", ndk_keys, 3);
  ccb_check_unknown_keys(v, "cmake", cmake_keys, 2);
  ccb_check_unknown_keys(v, "preview", preview_keys, 9);
  ccb_check_unknown_keys(v, "library", NULL, 0);
}

// Root: ccb_version.
static void ccb_load_root(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  long ver = 0;
  if (!ccb_get_int(v, "", "ccb_version", &ver)) {
    const ccb_toml_table *root = ccb_toml_find_table(doc, "");
    if (ccb_toml_find_value(root, "ccb_version") == NULL) {
      ccb_vdiag(v, "CCB-E1002", "missing required key 'ccb_version' (must be 1)");
    }
  } else if (ver != 1) {
    ccb_vdiag(v, "CCB-E1003", "unsupported ccb_version: only version 1 is supported");
  } else {
    c->ccb_version = ver;
  }
}

// A required non-empty string in [project]: missing is CCB-E1004,
// present-but-empty is CCB-E1005; a wrong type is already CCB-E1007
// from the getter and needs no further diagnostic.
static void ccb_load_project_str(ccb_validator *v, const ccb_toml_doc *doc,
    const char *key, char **field) {
  if (!ccb_get_str(v, "project", key, field) || (*field)[0] == '\0') {
    if (*field == NULL && !ccb_key_present(doc, "project", key)) {
      ccb_vdiagf(v, "CCB-E1004", "missing required key 'project.%s'", key, "");
    } else if (*field != NULL) {
      ccb_vdiagf(v, "CCB-E1005", "project.%s must not be empty", key, "");
    }
  }
}

// [project]
static void ccb_load_project(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  ccb_load_project_str(v, doc, "name", &c->name);
  ccb_load_project_str(v, doc, "package", &c->package);
  ccb_load_project_str(v, doc, "version", &c->version);
  {
    long vc = 0;
    if (ccb_get_int(v, "project", "version_code", &vc)) {
      if (vc < 0) {
        ccb_vdiag(v, "CCB-E1005", "project.version_code must be >= 0");
      } else {
        c->version_code = vc;
      }
    }
  }
  int have_min = ccb_get_int(v, "project", "min_sdk", &c->min_sdk);
  int have_target = ccb_get_int(v, "project", "target_sdk", &c->target_sdk);
  int have_compile = ccb_get_int(v, "project", "compile_sdk", &c->compile_sdk);
  if (!have_min && !ccb_key_present(doc, "project", "min_sdk")) {
    ccb_vdiag(v, "CCB-E1004", "missing required key 'project.min_sdk'");
  } else if (have_min && (c->min_sdk < 21 || c->min_sdk > 36)) {
    ccb_vdiag(v, "CCB-E1005", "project.min_sdk must be in 21..36");
  }
  if (!have_target && !ccb_key_present(doc, "project", "target_sdk")) {
    ccb_vdiag(v, "CCB-E1004", "missing required key 'project.target_sdk'");
  }
  if (!have_compile && !ccb_key_present(doc, "project", "compile_sdk")) {
    ccb_vdiag(v, "CCB-E1004", "missing required key 'project.compile_sdk'");
  } else if (have_compile && (c->compile_sdk < 34 || c->compile_sdk > 36)) {
    ccb_vdiag(v, "CCB-E1005", "project.compile_sdk must be 34, 35 or 36");
  }
  if (have_min && have_target && c->min_sdk > c->target_sdk) {
    ccb_vdiag(v, "CCB-E1005", "project.min_sdk must not exceed project.target_sdk");
  }
  if (have_target && have_compile && c->target_sdk > c->compile_sdk) {
    ccb_vdiag(v, "CCB-E1005", "project.target_sdk must not exceed project.compile_sdk");
  }
}

// [paths]
static void ccb_load_paths(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  char *m = NULL;
  if (!ccb_get_str(v, "paths", "manifest", &m)) {
    if (!ccb_key_present(doc, "paths", "manifest")) {
      ccb_vdiag(v, "CCB-E1004", "missing required key 'paths.manifest'");
    }
  } else {
    c->manifest = ccb_resolve(c->project_root, m);
    free(m);
  }
  ccb_load_resolved_str(v, c->project_root, "paths", "res", &c->res);
  ccb_load_resolved_str(v, c->project_root, "paths", "assets", &c->assets);
  ccb_load_resolved_str(v, c->project_root, "paths", "kotlin", &c->kotlin_dir);
  ccb_load_resolved_str(v, c->project_root, "paths", "java", &c->java_dir);
  if (ccb_get_str_array(v, "paths", "java_extra", &c->java_extra_dirs,
      &c->java_extra_dirs_count)) {
    for (size_t i = 0; i < c->java_extra_dirs_count; i++) {
      char *resolved = ccb_resolve(c->project_root, c->java_extra_dirs[i]);
      free(c->java_extra_dirs[i]);
      c->java_extra_dirs[i] = resolved;
    }
  }
}

// [sdk]
static void ccb_load_sdk(ccb_validator *v, ccb_config *c) {
  ccb_load_resolved_str(v, c->project_root, "sdk", "home", &c->sdk_home);
  ccb_load_replace_str(v, "sdk", "build_tools", &c->build_tools);
  ccb_load_plain_str(v, "sdk", "ndk_version", &c->ndk_version);
  ccb_load_resolved_str(v, c->project_root, "sdk", "kotlin_home", &c->kotlin_home);
  ccb_load_resolved_str(v, c->project_root, "sdk", "r8_jar", &c->r8_jar);
  // Tool overrides are stored exactly as written: a relative value
  // stays relative and is interpreted against the caller's working
  // directory when the tool runs, not against the project root or
  // the SDK home. No existence check here: ccb_sdk_resolve validates
  // the effective path (config, env, or default) at Stage 0.
  ccb_load_plain_str(v, "sdk", "aapt2", &c->aapt2_override);
  ccb_load_plain_str(v, "sdk", "zipalign", &c->zipalign_override);
  ccb_load_plain_str(v, "sdk", "adb", &c->adb_override);
}

// [signing.debug] and [signing.release]
static void ccb_load_signing(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  ccb_load_home_str(v, "signing.debug", "store", &c->debug_store);
  ccb_load_replace_str(v, "signing.debug", "alias", &c->debug_alias);
  if (ccb_toml_find_table(doc, "signing.release") == NULL) {
    return;
  }
  c->has_release_signing = 1;
  ccb_load_required_str(v, doc, "signing.release", "store",
    &c->release_store, 1);
  ccb_load_required_str(v, doc, "signing.release", "alias",
    &c->release_alias, 0);
  ccb_load_required_str(v, doc, "signing.release", "store_password_env",
    &c->release_store_pw_env, 0);
  ccb_load_required_str(v, doc, "signing.release", "key_password_env",
    &c->release_key_pw_env, 0);
}

// [build]
static void ccb_load_build(ccb_validator *v,
    ccb_config *c, const char *variant_override) {
  ccb_load_replace_str(v, "build", "variant", &c->variant);
  if (strcmp(c->variant, "debug") != 0 && strcmp(c->variant, "release") != 0) {
    ccb_vdiag(v, "CCB-E1005", "build.variant must be 'debug' or 'release'");
  }
  // The CLI --variant override applies before validation that depends
  // on the variant (e.g. minify requires release), so a project with
  // minify = true builds with --variant release even when the TOML
  // default variant is debug.
  if (variant_override != NULL) {
    if (strcmp(variant_override, "debug") != 0 &&
        strcmp(variant_override, "release") != 0) {
      ccb_vdiag(v, "CCB-E1005", "--variant must be 'debug' or 'release'");
    } else {
      free(c->variant);
      c->variant = ccb_xstrdup(variant_override);
    }
  }
  int b = 0;
  if (ccb_get_bool(v, "build", "kotlinc_daemon", &b)) {
    c->kotlinc_daemon = b;
  }
  long idle = 0;
  if (ccb_get_int(v, "build", "kotlinc_daemon_idle_seconds", &idle)) {
    c->kotlinc_daemon_idle_seconds = idle;
  }
  if (c->kotlinc_daemon_idle_seconds < CCB_DAEMON_IDLE_MIN_S ||
      c->kotlinc_daemon_idle_seconds > CCB_DAEMON_IDLE_MAX_S) {
    ccb_vdiag(v, "CCB-E1005",
      "build.kotlinc_daemon_idle_seconds must be between 60 and 3600");
  }
  if (ccb_get_bool(v, "build", "minify", &b)) {
    c->minify = b;
  }
  b = 0;
  if (ccb_get_bool(v, "build", "shrink_resources", &b)) {
    c->shrink_resources = b;
  }
  // shrink_resources is parsed but no task reads it: accepting true
  // would silently promise shrinking that never happens (Step 7
  // verdict: diagnose, don't ignore). False stays accepted.
  if (c->shrink_resources) {
    ccb_vdiag(v, "CCB-E1005",
      "build.shrink_resources is not supported (resource shrinking is not implemented)");
  }
  if (c->minify && strcmp(c->variant, "debug") == 0) {
    ccb_vdiag(v, "CCB-E1005", "build.minify requires variant = 'release'");
  }
  ccb_load_replace_str(v, "build", "jvm_target", &c->jvm_target);
  if (strcmp(c->jvm_target, "11") != 0 && strcmp(c->jvm_target, "17") != 0) {
    ccb_vdiag(v, "CCB-E1005", "build.jvm_target must be '11' or '17'");
  }
  ccb_load_resolved_str(v, c->project_root, "build", "proguard_rules",
    &c->proguard_rules);
  if (c->minify && c->proguard_rules == NULL) {
    ccb_vdiag(v, "CCB-E1004", "build.minify requires 'build.proguard_rules'");
  }
  ccb_get_str_array(v, "build", "kotlin_plugins", &c->kotlin_plugins,
    &c->kotlin_plugins_count);
  ccb_get_str_array(v, "build", "kotlin_plugin_options",
    &c->kotlin_plugin_options, &c->kotlin_plugin_options_count);
  for (size_t i = 0; i < c->kotlin_plugins_count; i++) {
    if (!ccb_config_plugin_coord_ok(c->kotlin_plugins[i])) {
      ccb_vdiagf(v, "CCB-E1005",
        "kotlin_plugins coordinate '%s' must be 'group:artifact:version' with an exact version (no ranges, LATEST, RELEASE, or -SNAPSHOT)",
        c->kotlin_plugins[i], "");
    }
  }
  for (size_t i = 0; i < c->kotlin_plugin_options_count; i++) {
    if (!ccb_config_plugin_option_ok(c->kotlin_plugin_options[i])) {
      ccb_vdiagf(v, "CCB-E1005",
        "kotlin_plugin_options entry '%s' must be 'plugin-id:key=value'",
        c->kotlin_plugin_options[i], "");
    }
  }
}

// Does a direct dependency coordinate carry this exclusion's version
// (or does the exclusion carry no version at all)? Coordinates here
// were already validated, so a missing second colon means no match.
static int ccb_maven_version_matches(const char *coord, const char *ev,
    size_t evlen) {
  const char *c2 = strchr(coord, ':');
  const char *c3 = c2 != NULL ? strchr(c2 + 1, ':') : NULL;
  if (c3 == NULL || strlen(c3 + 1) != evlen) {
    return 0;
  }
  return memcmp(c3 + 1, ev, evlen) == 0;
}

// Excluding a direct dependency is a config error: the exclusion
// would remove something the project explicitly declares. An
// unversioned exclusion matches every version; a versioned one
// matches only that exact version, so excluding some other version
// of a direct dependency is allowed.
static void ccb_deps_check_exclude(ccb_validator *v, ccb_config *c,
    const char *ex) {
  const char *eg = NULL;
  const char *ea = NULL;
  size_t eglen = 0;
  size_t ealen = 0;
  if (!ccb_config_maven_ga(ex, &eg, &eglen, &ea, &ealen)) {
    return;
  }
  const char *ec2 = strchr(ea, ':');
  const char *ev = ec2 != NULL ? ec2 + 1 : NULL;
  size_t evlen = ec2 != NULL ? strlen(ec2 + 1) : 0;
  for (size_t j = 0; j < c->maven_count; j++) {
    const char *dg = NULL;
    const char *da = NULL;
    size_t dglen = 0;
    size_t dalen = 0;
    if (!ccb_config_maven_ga(c->maven[j], &dg, &dglen, &da, &dalen)) {
      continue;
    }
    if (dglen != eglen || dalen != ealen || memcmp(dg, eg, dglen) != 0 ||
        memcmp(da, ea, dalen) != 0) {
      continue;
    }
    if (ev != NULL && !ccb_maven_version_matches(c->maven[j], ev, evlen)) {
      continue;
    }
    ccb_vdiagf(v, "CCB-E1005",
      "maven_exclude '%s' matches direct dependency '%s'; remove one of them",
      ex, c->maven[j]);
  }
}

// [deps]
static void ccb_load_deps(ccb_validator *v, ccb_config *c) {
  ccb_get_str_array(v, "deps", "jars", &c->jars, &c->jars_count);
  ccb_get_str_array(v, "deps", "aars", &c->aars, &c->aars_count);
  ccb_get_str_array(v, "deps", "maven", &c->maven, &c->maven_count);
  ccb_get_bool(v, "deps", "maven_central", &c->maven_central);
  ccb_get_str_array(v, "deps", "maven_exclude", &c->maven_exclude,
    &c->maven_exclude_count);
  ccb_get_str_array(v, "deps", "maven_repos", &c->maven_repos,
    &c->maven_repos_count);
  ccb_get_str_array(v, "deps", "maven_profiles", &c->maven_profiles,
    &c->maven_profiles_count);
  for (size_t i = 0; i < c->maven_count; i++) {
    if (!ccb_config_maven_coord_ok(c->maven[i])) {
      ccb_vdiagf(v, "CCB-E1005",
        "maven coordinate '%s' must be 'group:artifact:version' with an exact version, a version range, LATEST, RELEASE, or a -SNAPSHOT version",
        c->maven[i], "");
    }
  }
  for (size_t i = 0; i < c->maven_repos_count; i++) {
    if (!ccb_config_maven_repo_ok(c->maven_repos[i])) {
      ccb_vdiagf(v, "CCB-E1005",
        "maven_repos entry '%s' must be 'url' or 'url username password-source' (password-source 'env:VAR' or a literal password)",
        c->maven_repos[i], "");
    }
  }
  for (size_t i = 0; i < c->maven_exclude_count; i++) {
    const char *ex = c->maven_exclude[i];
    if (!ccb_config_maven_exclude_ok(ex)) {
      ccb_vdiagf(v, "CCB-E1005",
        "maven_exclude '%s' must be 'group:artifact' or 'group:artifact:version'",
        ex, "");
      continue;
    }
    ccb_deps_check_exclude(v, c, ex);
  }
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
}

// Is this one of the four ABIs CCB knows how to build for?
static int ccb_ndk_abi_known(const char *abi) {
  static const char *known_abis[] = { "arm64-v8a", "armeabi-v7a", "x86_64", "x86" };
  for (size_t j = 0; j < 4; j++) {
    if (strcmp(abi, known_abis[j]) == 0) {
      return 1;
    }
  }
  return 0;
}

// [ndk]
static void ccb_load_ndk(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  if (ccb_toml_find_table(doc, "ndk") == NULL) {
    return;
  }
  c->has_ndk = 1;
  ccb_get_str_array(v, "ndk", "abis", &c->ndk_abis, &c->ndk_abis_count);
  ccb_get_str_array(v, "ndk", "c_sources", &c->ndk_sources, &c->ndk_sources_count);
  ccb_get_str_array(v, "ndk", "cflags", &c->ndk_cflags, &c->ndk_cflags_count);
  if (c->ndk_abis_count == 0) {
    if (!ccb_key_present(doc, "ndk", "abis")) {
      ccb_vdiag(v, "CCB-E1004", "missing required key 'ndk.abis'");
    } else {
      ccb_vdiag(v, "CCB-E1005", "ndk.abis must not be empty");
    }
  }
  for (size_t i = 0; i < c->ndk_abis_count; i++) {
    if (!ccb_ndk_abi_known(c->ndk_abis[i])) {
      ccb_vdiagf(v, "CCB-E1005", "unknown ABI '%s' in ndk.abis", c->ndk_abis[i], "");
    }
  }
  for (size_t i = 0; i < c->ndk_sources_count; i++) {
    char *resolved = ccb_resolve(c->project_root, c->ndk_sources[i]);
    free(c->ndk_sources[i]);
    c->ndk_sources[i] = resolved;
  }
}

// [cmake]
static void ccb_load_cmake(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  if (ccb_toml_find_table(doc, "cmake") == NULL) {
    return;
  }
  c->has_cmake = 1;
  char *l = NULL;
  if (ccb_get_str(v, "cmake", "lists", &l)) {
    c->cmake_lists = ccb_resolve(c->project_root, l);
    free(l);
  } else if (!ccb_key_present(doc, "cmake", "lists")) {
    ccb_vdiag(v, "CCB-E1004", "missing required key 'cmake.lists'");
  }
  ccb_get_str_array(v, "cmake", "args", &c->cmake_args, &c->cmake_args_count);
  if (!c->has_ndk) {
    ccb_vdiag(v, "CCB-E1005", "[cmake] requires [ndk] abis");
  } else if (c->ndk_sources_count > 0) {
    ccb_vdiag(v, "CCB-E1005",
      "[cmake] replaces the [ndk] clang step: remove ndk.c_sources");
  }
}

// [preview.bindings]: code identifier -> component label, both
// strings. A non-string value is the existing wrong-type error
// (CCB-E1007); the table itself needs no unknown-key check
// because any identifier may be a key.
static void ccb_preview_bindings(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  const ccb_toml_table *bt = ccb_toml_find_table(doc, "preview.bindings");
  if (bt == NULL) {
    return;
  }
  for (size_t i = 0; i < bt->count; i++) {
    if (bt->kvs[i].value.type != CCB_TOML_STRING) {
      ccb_vdiagf(v, "CCB-E1007",
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

// [preview]
static void ccb_load_preview(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  if (ccb_toml_find_table(doc, "preview") == NULL) {
    return;
  }
  c->has_preview = 1;
  ccb_load_resolved_str(v, c->project_root, "preview", "styles",
    &c->preview_styles);
  ccb_load_resolved_str(v, c->project_root, "preview", "flow",
    &c->preview_flow);
  ccb_load_resolved_str(v, c->project_root, "preview", "resources",
    &c->preview_resources);
  ccb_load_resolved_str(v, c->project_root, "preview", "layout",
    &c->preview_layout);
  ccb_load_resolved_str(v, c->project_root, "preview", "nav_graph",
    &c->preview_nav_graph);
  long w = 0;
  if (ccb_get_int(v, "preview", "phone_width", &w)) {
    c->preview_phone_width = w;
  }
  long h = 0;
  if (ccb_get_int(v, "preview", "phone_height", &h)) {
    c->preview_phone_height = h;
  }
  int strict = 0;
  if (ccb_get_bool(v, "preview", "strict", &strict)) {
    c->preview_strict = strict;
  }
  int gate = 0;
  if (ccb_get_bool(v, "preview", "gate", &gate)) {
    c->preview_gate = gate;
  }
  ccb_preview_bindings(v, doc, c);
}

// ---------- [preview.scenario.*] ----------

// Find a scenario by name, or NULL.
static ccb_scenario *ccb_find_scenario(ccb_config *c, const char *name) {
  for (size_t i = 0; i < c->preview_scenario_count; i++) {
    if (strcmp(c->preview_scenarios[i].name, name) == 0) {
      return &c->preview_scenarios[i];
    }
  }
  return NULL;
}

// Find the scenario a section table belongs to: the scenario whose
// name is the table's first segment. Names never contain '.', so a
// prefix match on the first segment is an exact match.
static ccb_scenario *ccb_find_scenario_for_table(ccb_config *c,
    const char *rest) {
  for (size_t i = 0; i < c->preview_scenario_count; i++) {
    size_t nlen = strlen(c->preview_scenarios[i].name);
    if (strlen(rest) > nlen &&
        strncmp(rest, c->preview_scenarios[i].name, nlen) == 0 &&
        rest[nlen] == '.') {
      return &c->preview_scenarios[i];
    }
  }
  return NULL;
}

// A scenario name is a single path-safe segment.
static int ccb_scenario_name_ok(const char *name) {
  return name[0] != '\0' && strcmp(name, ".") != 0 &&
    strcmp(name, "..") != 0 && strchr(name, '/') == NULL &&
    strchr(name, '\\') == NULL;
}

// Append a new empty scenario, taking ownership of name. The
// returned pointer is valid until the next scenario is created
// (appending may move the array).
static ccb_scenario *ccb_scenario_new(ccb_config *c, char *name) {
  c->preview_scenarios = ccb_xrealloc(c->preview_scenarios,
    (c->preview_scenario_count + 1) *
    sizeof(*c->preview_scenarios));
  ccb_scenario *sc = &c->preview_scenarios[c->preview_scenario_count];
  memset(sc, 0, sizeof(*sc));
  sc->name = name;
  c->preview_scenario_count++;
  return sc;
}

// A bare [preview.scenario.<name>] table carries no data: it
// registers the name (an empty scenario is a valid no-op), and keys
// under it are CCB-E1001, like any unknown key.
static void ccb_scenario_bare_table(ccb_validator *v,
    const ccb_toml_doc *doc, ccb_config *c, const char *tname,
    const char *rest) {
  if (ccb_find_scenario(c, rest) == NULL) {
    if (!ccb_scenario_name_ok(rest)) {
      ccb_vdiagf(v, "CCB-E1005", "invalid scenario name '%s'", rest, "");
      return;
    }
    ccb_scenario_new(c, ccb_xstrdup(rest));
  }
  const ccb_toml_table *t = ccb_toml_find_table(doc, tname);
  for (size_t i = 0; i < t->count; i++) {
    ccb_vdiagf(v, "CCB-E1001",
      "unknown key '%s' in table [%s]",
      t->kvs[i].key, tname);
  }
}

// One section of a scenario: branches and fields hold strings,
// lists and args hold string arrays.
static void ccb_scenario_section(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_scenario *sc, const char *tname, const char *section) {
  const ccb_toml_table *t = ccb_toml_find_table(doc, tname);
  if (strcmp(section, "branches") == 0) {
    for (size_t i = 0; i < t->count; i++) {
      char *entry = NULL;
      const char *screen = t->kvs[i].key;
      if (!ccb_get_str(v, tname, screen, &entry)) {
        continue;
      }
      sc->branches = ccb_xrealloc(sc->branches,
        (sc->nbranches + 1) * sizeof(*sc->branches));
      sc->branches[sc->nbranches].screen = ccb_xstrdup(screen);
      sc->branches[sc->nbranches].entry = entry;
      sc->nbranches++;
    }
    return;
  }
  if (strcmp(section, "fields") == 0) {
    for (size_t i = 0; i < t->count; i++) {
      char *text = NULL;
      const char *label = t->kvs[i].key;
      if (!ccb_get_str(v, tname, label, &text)) {
        continue;
      }
      sc->fields = ccb_xrealloc(sc->fields,
        (sc->nfields + 1) * sizeof(*sc->fields));
      sc->fields[sc->nfields].label = ccb_xstrdup(label);
      sc->fields[sc->nfields].text = text;
      sc->nfields++;
    }
    return;
  }
  if (strcmp(section, "lists") == 0) {
    for (size_t i = 0; i < t->count; i++) {
      char **rows = NULL;
      size_t nrows = 0;
      const char *label = t->kvs[i].key;
      if (!ccb_get_str_array(v, tname, label, &rows, &nrows)) {
        continue;
      }
      sc->lists = ccb_xrealloc(sc->lists,
        (sc->nlists + 1) * sizeof(*sc->lists));
      sc->lists[sc->nlists].label = ccb_xstrdup(label);
      sc->lists[sc->nlists].rows = rows;
      sc->lists[sc->nlists].nrows = nrows;
      sc->nlists++;
    }
    return;
  }
  if (strcmp(section, "args") == 0) {
    for (size_t i = 0; i < t->count; i++) {
      char **values = NULL;
      size_t nvalues = 0;
      const char *label = t->kvs[i].key;
      if (!ccb_get_str_array(v, tname, label, &values, &nvalues)) {
        continue;
      }
      sc->args = ccb_xrealloc(sc->args,
        (sc->nargs + 1) * sizeof(*sc->args));
      sc->args[sc->nargs].label = ccb_xstrdup(label);
      sc->args[sc->nargs].values = values;
      sc->args[sc->nargs].nvalues = nvalues;
      sc->nargs++;
    }
    return;
  }
  ccb_vdiagf(v, "CCB-E1001",
    "unknown scenario section '%s' in [preview.scenario.%s]",
    section, sc->name);
}

// One [preview.scenario.*] table: a bare table or one section of a
// scenario. The scenario is found or created by its single-segment
// name; an invalid name is CCB-E1005 and the table is skipped.
static void ccb_scenario_table(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c, const char *tname) {
  static const char prefix[] = "preview.scenario.";
  size_t plen = strlen(prefix);
  const char *rest = tname + plen;
  const char *dot = strchr(rest, '.');
  if (dot == NULL) {
    ccb_scenario_bare_table(v, doc, c, tname, rest);
    return;
  }
  ccb_scenario *sc = ccb_find_scenario_for_table(c, rest);
  if (sc == NULL) {
    size_t nlen = (size_t)(dot - rest);
    char *name = ccb_xmalloc(nlen + 1);
    memcpy(name, rest, nlen);
    name[nlen] = '\0';
    if (!ccb_scenario_name_ok(name)) {
      ccb_vdiagf(v, "CCB-E1005", "invalid scenario name '%s'", name, "");
      free(name);
      return;
    }
    sc = ccb_scenario_new(c, name);
  }
  ccb_scenario_section(v, doc, sc, tname, dot + 1);
}

// [preview.scenario.<name>.<section>] tables, sub-table spelling:
// branches maps screen -> entry label (strings), lists maps list
// label -> rows (string arrays), fields maps textfield label ->
// text (strings), args maps button label -> values (string
// arrays). The TOML subset has no inline tables, so the plan's
// `branches = {...}` example is written as a section table
// instead; the data model is identical. A bare
// [preview.scenario.<name>] table carries no data: keys under it
// are CCB-E1001, like any unknown key. Wrong value types are
// CCB-E1007 via the shared getters. Parsed regardless of whether
// [preview] exists: the preview-applicability gate, not the
// config, decides whether scenarios ever run.
static void ccb_load_scenarios(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  static const char prefix[] = "preview.scenario.";
  size_t plen = strlen(prefix);
  for (size_t ti = 0; ti < doc->count; ti++) {
    if (strncmp(doc->tables[ti].name, prefix, plen) != 0) {
      continue;
    }
    ccb_scenario_table(v, doc, c, doc->tables[ti].name);
  }
}

// ---------- [modules.<name>] ----------

// Optional standard-layout inputs: present only when the directory
// exists on disk.
static void ccb_module_layout_inputs(ccb_module *m) {
  char *p = ccb_path_join3(m->dir, "src/main", "res");
  if (ccb_path_is_dir(p)) {
    m->res = p;
  } else {
    free(p);
  }
  p = ccb_path_join3(m->dir, "src/main", "assets");
  if (ccb_path_is_dir(p)) {
    m->assets = p;
  } else {
    free(p);
  }
  p = ccb_path_join3(m->dir, "src/main", "kotlin");
  if (ccb_path_is_dir(p)) {
    m->kotlin_dir = p;
  } else {
    free(p);
  }
  p = ccb_path_join3(m->dir, "src/main", "java");
  if (ccb_path_is_dir(p)) {
    m->java_dir = p;
  } else {
    free(p);
  }
}

// A module manifest must declare a package, it must differ from the
// app package, and no two modules may share one. m->package is NULL
// or empty when the manifest was unreadable or declares none.
static void ccb_module_check_package(ccb_validator *v, ccb_config *c,
    ccb_module *m, const char *mod_name, size_t mi) {
  if (m->package == NULL || m->package[0] == '\0') {
    ccb_vdiagf(v, "CCB-E1005",
      "module '%s' manifest must declare a package attribute",
      mod_name, "");
    return;
  }
  if (c->package != NULL && strcmp(m->package, c->package) == 0) {
    ccb_vdiagf(v, "CCB-E1005",
      "module '%s' package must differ from the app package",
      mod_name, "");
    return;
  }
  for (size_t k = 0; k < mi; k++) {
    if (c->modules[k].package != NULL &&
        strcmp(c->modules[k].package, m->package) == 0) {
      ccb_vdiagf(v, "CCB-E1005",
        "modules '%s' and '%s' share a package",
        c->modules[k].name, mod_name);
      break;
    }
  }
}

// Register one [modules.<name>] table. The standard layout under
// <dir> mirrors [paths]: src/main/AndroidManifest.xml (required),
// src/main/res, src/main/assets, src/main/kotlin, src/main/java
// (each optional; absent means the module has no such input).
static void ccb_module_register(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c, const char *tname, const char *mod_name) {
  static const char *module_keys[] = { "dir", "depends" };
  ccb_check_unknown_keys(v, tname, module_keys, 2);
  size_t mi = c->modules_count++;
  c->modules = ccb_xrealloc(c->modules,
    c->modules_count * sizeof(*c->modules));
  ccb_module *m = &c->modules[mi];
  memset(m, 0, sizeof(*m));
  m->name = ccb_xstrdup(mod_name);
  char *dir = NULL;
  if (!ccb_get_str(v, tname, "dir", &dir)) {
    if (!ccb_key_present(doc, tname, "dir")) {
      ccb_vdiagf(v, "CCB-E1004", "missing required key '%s.dir'", tname, "");
    }
  } else {
    m->dir = ccb_resolve(c->project_root, dir);
    free(dir);
  }
  ccb_get_str_array(v, tname, "depends", &m->depends,
    &m->depends_count);
  if (m->dir == NULL) {
    return;
  }
  if (!ccb_path_is_dir(m->dir)) {
    ccb_vdiagf(v, "CCB-E1006", "module directory does not exist: %s",
      m->dir, "");
    return;
  }
  m->manifest = ccb_path_join3(m->dir, "src/main",
    "AndroidManifest.xml");
  if (!ccb_path_exists(m->manifest) || ccb_path_is_dir(m->manifest)) {
    ccb_vdiagf(v, "CCB-E1006", "module manifest does not exist: %s",
      m->manifest, "");
  } else {
    char *text = NULL;
    size_t len = 0;
    if (ccb_read_file(m->manifest, &text, &len) == CCB_OK) {
      m->package = ccb_manifest_package(text);
      free(text);
    }
    ccb_module_check_package(v, c, m, mod_name, mi);
  }
  ccb_module_layout_inputs(m);
}

// [modules.<name>] library modules.
static void ccb_load_modules(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c) {
  for (size_t ti = 0; ti < doc->count; ti++) {
    const char *tname = doc->tables[ti].name;
    if (strcmp(tname, "modules") == 0) {
      ccb_vdiagf(v, "CCB-E1001", "unknown table '[%s]'", tname, "");
      continue;
    }
    if (strncmp(tname, "modules.", 8) != 0) {
      continue;
    }
    const char *mod_name = tname + 8;
    if (mod_name[0] == '\0' || strchr(mod_name, '.') != NULL) {
      ccb_vdiagf(v, "CCB-E1001", "unknown table '[%s]'", tname, "");
      continue;
    }
    if (strchr(mod_name, '/') != NULL || strchr(mod_name, '\\') != NULL ||
        strcmp(mod_name, ".") == 0 || strcmp(mod_name, "..") == 0) {
      ccb_vdiagf(v, "CCB-E1005", "invalid module name '%s'", mod_name, "");
      continue;
    }
    if (ccb_find_module(c, mod_name) != NULL) {
      ccb_vdiagf(v, "CCB-E1005", "duplicate module '%s'", mod_name, "");
      continue;
    }
    ccb_module_register(v, doc, c, tname, mod_name);
  }
  ccb_sort_modules(v, c);
}

// ---------- [library] + [deps.library.<name>] ----------

// The [library] table marks a project as a library: it builds an .aar,
// not an APK. The table takes no keys; subtables are unknown tables.
static void ccb_load_library_table(ccb_validator *v,
    const ccb_toml_doc *doc, ccb_config *c) {
  if (ccb_toml_find_table(doc, "library") == NULL) {
    return;
  }
  c->is_library = 1;
  ccb_check_unknown_keys(v, "library", NULL, 0);
  for (size_t ti = 0; ti < doc->count; ti++) {
    if (strncmp(doc->tables[ti].name, "library.", 8) == 0) {
      ccb_vdiagf(v, "CCB-E1001", "unknown table '[%s]'",
        doc->tables[ti].name, "");
    }
  }
}

// Package of an AAR's AndroidManifest.xml, or NULL when the AAR has no
// readable manifest.
static char *ccb_aar_package(const char *aar_path) {
  ccb_zip_archive *a = NULL;
  if (ccb_zip_open(aar_path, &a) != CCB_OK) {
    return NULL;
  }
  char *pkg = NULL;
  const ccb_zip_entry *e = ccb_zip_find(a, "AndroidManifest.xml");
  if (e != NULL) {
    void *data = NULL;
    size_t len = 0;
    if (ccb_zip_read_entry(a, e, &data, &len) == CCB_OK) {
      char *text = ccb_xmalloc(len + 1);
      memcpy(text, data, len);
      text[len] = '\0';
      pkg = ccb_manifest_package(text);
      free(text);
      free(data);
    }
  }
  ccb_zip_close(a);
  return pkg;
}

// A path library's package must exist and differ from the app package,
// every module package, and every earlier library package.
static void ccb_library_check_package(ccb_validator *v, ccb_config *c,
    ccb_module *m, const char *lib_name, size_t li) {
  if (m->package == NULL || m->package[0] == '\0') {
    ccb_vdiagf(v, "CCB-E1005",
      "library '%s' manifest must declare a package attribute",
      lib_name, "");
    return;
  }
  if (c->package != NULL && strcmp(m->package, c->package) == 0) {
    ccb_vdiagf(v, "CCB-E1005",
      "library '%s' package must differ from the app package",
      lib_name, "");
    return;
  }
  for (size_t k = 0; k < c->modules_count; k++) {
    if (c->modules[k].package != NULL &&
        strcmp(c->modules[k].package, m->package) == 0) {
      ccb_vdiagf(v, "CCB-E1005",
        "library '%s' and module '%s' share a package",
        lib_name, c->modules[k].name);
      return;
    }
  }
  for (size_t k = 0; k < li; k++) {
    if (c->libraries[k].package != NULL &&
        strcmp(c->libraries[k].package, m->package) == 0) {
      ccb_vdiagf(v, "CCB-E1005",
        "libraries '%s' and '%s' share a package",
        c->libraries[k].name, lib_name);
      return;
    }
  }
}

// One string value from a parsed library ccb.toml (table.key). Missing
// keys are CCB-E1004, wrong types CCB-E1007; the message names the
// library file. Returns 1 on success, 0 on failure (*out untouched).
static int ccb_lib_get_str(ccb_validator *v, const ccb_toml_doc *doc,
    const char *lib_toml, const char *table, const char *key,
    char **out) {
  const ccb_toml_table *t = ccb_toml_find_table(doc, table);
  const ccb_toml_value *val = ccb_toml_find_value(t, key);
  if (val == NULL) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "library '");
    ccb_str_append(&msg, lib_toml);
    ccb_str_append(&msg, "' is missing required key '");
    ccb_str_append(&msg, table);
    ccb_str_append(&msg, ".");
    ccb_str_append(&msg, key);
    ccb_str_append(&msg, "'");
    ccb_diag d = ccb_diag_make("CCB-E1004", "config", msg.buf);
    ccb_str_free(&msg);
    ccb_diag_list_push(v->diags, &d);
    return 0;
  }
  if (val->type != CCB_TOML_STRING) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "library '");
    ccb_str_append(&msg, lib_toml);
    ccb_str_append(&msg, "' key '");
    ccb_str_append(&msg, table);
    ccb_str_append(&msg, ".");
    ccb_str_append(&msg, key);
    ccb_str_append(&msg, "' must be a string");
    ccb_diag d = ccb_diag_make("CCB-E1007", "config", msg.buf);
    ccb_str_free(&msg);
    ccb_diag_list_push(v->diags, &d);
    return 0;
  }
  *out = ccb_xstrdup(val->str);
  return 1;
}

// Optional string value from a parsed library ccb.toml: NULL when the
// key is absent (wrong types are still an error, as above).
static int ccb_lib_get_opt_str(ccb_validator *v, const ccb_toml_doc *doc,
    const char *lib_toml, const char *table, const char *key,
    char **out) {
  const ccb_toml_table *t = ccb_toml_find_table(doc, table);
  const ccb_toml_value *val = ccb_toml_find_value(t, key);
  *out = NULL;
  if (val == NULL) {
    return 1;
  }
  return ccb_lib_get_str(v, doc, lib_toml, table, key, out);
}

// Standard-layout inputs under a library project root: the manifest is
// required, res/assets/kotlin/java are present only when the directory
// exists on disk. Paths in [paths] override the standard layout.
static void ccb_library_layout(ccb_validator *v, const ccb_toml_doc *doc,
    const char *lib_toml, ccb_module *m) {
  char *p = NULL;
  if (!ccb_lib_get_opt_str(v, doc, lib_toml, "paths", "manifest", &p)) {
    return;
  }
  if (p != NULL) {
    m->manifest = ccb_resolve(m->dir, p);
    free(p);
  } else {
    m->manifest = ccb_path_join3(m->dir, "src/main",
      "AndroidManifest.xml");
  }
  if (!ccb_path_exists(m->manifest) || ccb_path_is_dir(m->manifest)) {
    ccb_vdiagf(v, "CCB-E1006", "library manifest does not exist: %s",
      m->manifest, "");
    return;
  }
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(m->manifest, &text, &len) == CCB_OK) {
    m->package = ccb_manifest_package(text);
    free(text);
  }
  const char *keys[4] = { "res", "assets", "kotlin", "java" };
  char **slots[4] = { &m->res, &m->assets, &m->kotlin_dir, &m->java_dir };
  for (size_t i = 0; i < 4; i++) {
    char *rel = NULL;
    if (!ccb_lib_get_opt_str(v, doc, lib_toml, "paths", keys[i], &rel)) {
      return;
    }
    char *cand = NULL;
    if (rel != NULL) {
      cand = ccb_resolve(m->dir, rel);
      free(rel);
    } else {
      cand = ccb_path_join3(m->dir, "src/main", keys[i]);
    }
    if (ccb_path_is_dir(cand)) {
      *slots[i] = cand;
    } else {
      free(cand);
    }
  }
}

// Register one library project dir: read its ccb.toml minimally (no
// recursive config load, so a library cycle cannot recurse). The
// library must declare [library], a [project] name/version/package,
// and a manifest whose package matches [project] package; it must not
// declare its own deps, modules, or NDK (a path library is built with
// the app's toolchain and classpath).
static void ccb_library_register_project(ccb_validator *v,
    ccb_config *c, ccb_module *m, size_t li, const char *lib_name) {
  char *lib_toml = ccb_path_join(m->dir, "ccb.toml");
  if (!ccb_path_exists(lib_toml) || ccb_path_is_dir(lib_toml)) {
    ccb_vdiagf(v, "CCB-E1006", "library project has no ccb.toml: %s",
      lib_toml, "");
    free(lib_toml);
    return;
  }
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(lib_toml, &text, &len) != CCB_OK) {
    ccb_vdiagf(v, "CCB-E1006", "cannot read library ccb.toml: %s",
      lib_toml, "");
    free(lib_toml);
    return;
  }
  ccb_toml_error *perrors = NULL;
  size_t nperrors = 0;
  ccb_toml_doc *doc = ccb_toml_parse(text, &perrors, &nperrors);
  free(text);
  if (doc == NULL) {
    for (size_t i = 0; i < nperrors; i++) {
      ccb_diag d = ccb_diag_make("CCB-E1008", "config", perrors[i].message);
      d.file = ccb_xstrdup(lib_toml);
      d.line = perrors[i].line;
      ccb_diag_list_push(v->diags, &d);
    }
    ccb_toml_free_errors(perrors, nperrors);
    free(lib_toml);
    return;
  }
  if (ccb_toml_find_table(doc, "library") == NULL) {
    ccb_vdiagf(v, "CCB-E1005",
      "library path '%s' is not a library project (no [library] table)",
      m->dir, "");
    ccb_toml_free_doc(doc);
    free(lib_toml);
    return;
  }
  const ccb_toml_table *deps = ccb_toml_find_table(doc, "deps");
  if (deps != NULL && (ccb_toml_find_value(deps, "jars") != NULL ||
      ccb_toml_find_value(deps, "aars") != NULL ||
      ccb_toml_find_value(deps, "maven") != NULL)) {
    ccb_vdiagf(v, "CCB-E1005",
      "library '%s' cannot declare its own [deps] jars/aars/maven",
      lib_name, "");
    ccb_toml_free_doc(doc);
    free(lib_toml);
    return;
  }
  int bad = 0;
  for (size_t ti = 0; ti < doc->count; ti++) {
    if (strncmp(doc->tables[ti].name, "modules.", 8) == 0 ||
        strcmp(doc->tables[ti].name, "ndk") == 0) {
      bad = 1;
      break;
    }
  }
  if (bad) {
    ccb_vdiagf(v, "CCB-E1005",
      "library '%s' cannot declare modules or [ndk]", lib_name, "");
    ccb_toml_free_doc(doc);
    free(lib_toml);
    return;
  }
  char *proj_name = NULL;
  char *proj_version = NULL;
  char *proj_package = NULL;
  if (!ccb_lib_get_str(v, doc, lib_toml, "project", "name", &proj_name) ||
      !ccb_lib_get_str(v, doc, lib_toml, "project", "version",
        &proj_version) ||
      !ccb_lib_get_str(v, doc, lib_toml, "project", "package",
        &proj_package)) {
    free(proj_name);
    free(proj_version);
    free(proj_package);
    ccb_toml_free_doc(doc);
    free(lib_toml);
    return;
  }
  free(proj_name);
  m->version = proj_version;
  ccb_library_layout(v, doc, lib_toml, m);
  if (m->package != NULL && proj_package != NULL &&
      strcmp(m->package, proj_package) != 0) {
    ccb_vdiagf(v, "CCB-E1005",
      "library '%s' manifest package does not match its project.package",
      lib_name, "");
  }
  free(proj_package);
  ccb_toml_free_doc(doc);
  free(lib_toml);
  if (m->package != NULL) {
    ccb_library_check_package(v, c, m, lib_name, li);
  }
}

// Register one [deps.library.<name>] table: path is a library project
// dir or a built .aar file.
static void ccb_library_register(ccb_validator *v, const ccb_toml_doc *doc,
    ccb_config *c, const char *tname, const char *lib_name) {
  static const char *library_keys[] = { "path" };
  ccb_check_unknown_keys(v, tname, library_keys, 1);
  size_t li = c->libraries_count++;
  c->libraries = ccb_xrealloc(c->libraries,
    c->libraries_count * sizeof(*c->libraries));
  ccb_module *m = &c->libraries[li];
  memset(m, 0, sizeof(*m));
  m->name = ccb_xstrdup(lib_name);
  char *path = NULL;
  if (!ccb_get_str(v, tname, "path", &path)) {
    if (!ccb_key_present(doc, tname, "path")) {
      ccb_vdiagf(v, "CCB-E1004", "missing required key '%s.path'", tname, "");
    }
  } else {
    m->dir = ccb_resolve(c->project_root, path);
    free(path);
  }
  if (m->dir == NULL) {
    return;
  }
  if (!ccb_path_exists(m->dir)) {
    ccb_vdiagf(v, "CCB-E1006", "library path does not exist: %s",
      m->dir, "");
    return;
  }
  if (ccb_path_is_dir(m->dir)) {
    ccb_library_register_project(v, c, m, li, lib_name);
    return;
  }
  size_t len = strlen(m->dir);
  if (len < 5 || strcmp(m->dir + len - 4, ".aar") != 0) {
    ccb_vdiagf(v, "CCB-E1005",
      "library path '%s' is neither a directory nor an .aar file",
      m->dir, "");
    return;
  }
  m->is_file = 1;
  m->package = ccb_aar_package(m->dir);
  ccb_library_check_package(v, c, m, lib_name, li);
}

// [deps.library.<name>] path libraries.
static void ccb_load_dep_libraries(ccb_validator *v,
    const ccb_toml_doc *doc, ccb_config *c) {
  for (size_t ti = 0; ti < doc->count; ti++) {
    const char *tname = doc->tables[ti].name;
    if (strcmp(tname, "deps.library") == 0) {
      ccb_vdiagf(v, "CCB-E1001", "unknown table '[%s]'", tname, "");
      continue;
    }
    if (strncmp(tname, "deps.library.", 13) != 0) {
      continue;
    }
    const char *lib_name = tname + 13;
    if (lib_name[0] == '\0' || strchr(lib_name, '.') != NULL) {
      ccb_vdiagf(v, "CCB-E1001", "unknown table '[%s]'", tname, "");
      continue;
    }
    if (strchr(lib_name, '/') != NULL || strchr(lib_name, '\\') != NULL ||
        strcmp(lib_name, ".") == 0 || strcmp(lib_name, "..") == 0) {
      ccb_vdiagf(v, "CCB-E1005", "invalid library name '%s'", lib_name, "");
      continue;
    }
    int dup = 0;
    for (size_t i = 0; i < c->libraries_count; i++) {
      if (strcmp(c->libraries[i].name, lib_name) == 0) {
        dup = 1;
        break;
      }
    }
    if (dup) {
      ccb_vdiagf(v, "CCB-E1005", "duplicate library '%s'", lib_name, "");
      continue;
    }
    if (ccb_find_module(c, lib_name) != NULL) {
      ccb_vdiagf(v, "CCB-E1005",
        "library '%s' collides with a module of the same name",
        lib_name, "");
      continue;
    }
    ccb_library_register(v, doc, c, tname, lib_name);
  }
}

// A library project cannot contain modules or NDK, declare its own
// deps, or consume path libraries: it is built with the consuming
// app's toolchain and classpath.
static void ccb_check_library_rules(ccb_validator *v, ccb_config *c) {
  if (!c->is_library) {
    return;
  }
  if (c->modules_count > 0) {
    ccb_vdiag(v, "CCB-E1005",
      "library projects cannot declare [modules.<name>] tables");
  }
  if (c->has_ndk) {
    ccb_vdiag(v, "CCB-E1005", "library projects cannot declare [ndk]");
  }
  if (c->jars_count > 0 || c->aars_count > 0 || c->maven_count > 0) {
    ccb_vdiag(v, "CCB-E1005",
      "library projects cannot declare [deps] jars/aars/maven");
  }
  if (c->libraries_count > 0) {
    ccb_vdiag(v, "CCB-E1005",
      "library projects cannot consume path libraries");
  }
}

// Path existence checks (only when no earlier error already explains
// the path, and only for declared paths).
static void ccb_check_paths(ccb_validator *v, ccb_config *c) {
  if (c->manifest != NULL) {
    ccb_require_file(v, c->manifest, "manifest");
  }
  if (c->res != NULL) {
    ccb_require_dir(v, c->res, "res directory");
  }
  if (c->assets != NULL) {
    ccb_require_dir(v, c->assets, "assets directory");
  }
  if (c->kotlin_dir != NULL) {
    ccb_require_dir(v, c->kotlin_dir, "kotlin directory");
  }
  if (c->java_dir != NULL) {
    ccb_require_dir(v, c->java_dir, "java directory");
  }
  for (size_t i = 0; i < c->java_extra_dirs_count; i++) {
    ccb_require_dir(v, c->java_extra_dirs[i], "java_extra directory");
  }
  if (c->cmake_lists != NULL) {
    ccb_require_file(v, c->cmake_lists, "cmake lists file");
  }
  if (c->sdk_home != NULL) {
    ccb_require_dir(v, c->sdk_home, "sdk home");
  }
  if (c->kotlin_home != NULL) {
    ccb_require_dir(v, c->kotlin_home, "kotlin home");
  }
  if (c->r8_jar != NULL) {
    ccb_require_file(v, c->r8_jar, "r8 jar");
  }
  if (c->proguard_rules != NULL) {
    ccb_require_file(v, c->proguard_rules, "proguard rules");
  }
  if (c->debug_store != NULL) {
    ccb_require_file(v, c->debug_store, "debug keystore");
  }
  if (c->release_store != NULL) {
    ccb_require_file(v, c->release_store, "release keystore");
  }
  for (size_t i = 0; i < c->jars_count; i++) {
    ccb_require_file(v, c->jars[i], "jar dependency");
  }
  for (size_t i = 0; i < c->aars_count; i++) {
    ccb_require_file(v, c->aars[i], "aar dependency");
  }
  for (size_t i = 0; i < c->ndk_sources_count; i++) {
    ccb_require_file(v, c->ndk_sources[i], "ndk source");
  }
  if (c->preview_styles != NULL) {
    ccb_require_dir(v, c->preview_styles, "preview styles directory");
  }
  if (c->preview_flow != NULL) {
    ccb_require_dir(v, c->preview_flow, "preview flow directory");
  }
  if (c->preview_resources != NULL) {
    ccb_require_dir(v, c->preview_resources, "preview resources directory");
  }
  if (c->preview_layout != NULL) {
    ccb_require_path(v, c->preview_layout, "preview layout path");
  }
  if (c->preview_nav_graph != NULL) {
    ccb_require_path(v, c->preview_nav_graph, "preview nav_graph path");
  }
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
  c->kotlinc_daemon_idle_seconds = CCB_DAEMON_IDLE_DEFAULT_S;
  c->build_dir = ccb_path_join(c->project_root, "build");
  c->preview_phone_width = 390;
  c->preview_phone_height = 844;

  ccb_load_unknown_keys(&v);
  ccb_load_root(&v, doc, c);
  ccb_load_project(&v, doc, c);
  ccb_load_paths(&v, doc, c);
  ccb_load_sdk(&v, c);
  ccb_load_signing(&v, doc, c);
  ccb_load_build(&v, c, variant_override);
  ccb_load_deps(&v, c);
  ccb_load_ndk(&v, doc, c);
  ccb_load_cmake(&v, doc, c);
  ccb_load_preview(&v, doc, c);
  ccb_load_scenarios(&v, doc, c);
  ccb_load_modules(&v, doc, c);
  ccb_load_library_table(&v, doc, c);
  ccb_load_dep_libraries(&v, doc, c);
  ccb_check_library_rules(&v, c);
  ccb_check_paths(&v, c);

  ccb_toml_free_doc(doc);
  if (diags->count > diag_before) {
    ccb_config_free(c);
    return CCB_ERR_CONFIG;
  }
  *out = c;
  return CCB_OK;
}
