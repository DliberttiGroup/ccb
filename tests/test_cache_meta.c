// Tests for the audited cache-fingerprint table (Step 3):
// docs/cache-fingerprints.md must agree with the code, and every
// config key must have a coverage decision. Fast: no toolchain, only
// reads the repo tree (the doc, src/ccb_config.h, src/ccb_pipeline*.c
// and src/ccb_pipeline.c). The repo root is found from this test
// binary's own path (it lives in build/, next to the checkout root),
// with CWD fallbacks for manual runs.
//
// What is checked, and why it is not circular: the doc and the code
// are independent artifacts. The test compares them mechanically —
// literal fingerprint keys per task (from ccb_fp_add calls, grouped
// by enclosing function), config-field coverage (every struct field
// must have a table row, every row must name a real field), and cache
// step coverage (every ccb_cache_step_fp/set_step call site must
// resolve to a documented task). A new config key with no fingerprint
// decision, a new fingerprint key with no doc row, or a new cache step
// with no doc section all fail here. Behavior (does the fingerprint
// actually invalidate?) is proved by tests/test_cache_regression.c,
// not here.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include <stdlib.h>

#define MAX_SECTIONS 24
#define MAX_KEYS 40
#define MAX_FIELDS 160
#define MAX_STEPS 48
#define MAX_LINES 8192

static char ROOT[4096];

// ---------- small string helpers ----------

static int is_space_c(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_ident_c(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
    (c >= '0' && c <= '9') || c == '_';
}

static int starts_with(const char *s, const char *pre) {
  return strncmp(s, pre, strlen(pre)) == 0;
}

// Trim in place; returns the first non-space char.
static char *trim(char *s) {
  while (is_space_c(*s)) {
    s++;
  }
  size_t n = strlen(s);
  while (n > 0 && is_space_c(s[n - 1])) {
    s[--n] = '\0';
  }
  return s;
}

// First whitespace-delimited word of s, copied out.
static void first_word(const char *s, char *out, size_t n) {
  while (is_space_c(*s)) {
    s++;
  }
  size_t i = 0;
  while (s[i] != '\0' && !is_space_c(s[i]) && i + 1 < n) {
    out[i] = s[i];
    i++;
  }
  out[i] = '\0';
}

// ---------- line splitting ----------

typedef struct {
  char **lines;
  size_t count;
} line_list;

static void split_lines(char *text, line_list *out) {
  out->lines = ccb_xcalloc(MAX_LINES, sizeof(*out->lines));
  out->count = 0;
  char *p = text;
  while (*p != '\0' && out->count < MAX_LINES) {
    out->lines[out->count++] = p;
    char *nl = strchr(p, '\n');
    if (nl == NULL) {
      break;
    }
    *nl = '\0';
    p = nl + 1;
  }
  CHECK(out->count < MAX_LINES);
}

// ---------- repo root ----------

static void find_repo_root(void) {
  char exe[4096];
  ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  char cands[3][4096];
  size_t ncand = 0;
  if (k > 0) {
    exe[k] = '\0';
    char *dir = ccb_dirname(exe);
    snprintf(cands[ncand], sizeof(cands[0]), "%s/..", dir);
    free(dir);
    ncand++;
  }
  snprintf(cands[ncand++], sizeof(cands[0]), ".");
  snprintf(cands[ncand++], sizeof(cands[0]), "..");
  for (size_t i = 0; i < ncand; i++) {
    char *probe = ccb_path_join(cands[i], "docs/cache-fingerprints.md");
    if (access(probe, R_OK) == 0) {
      snprintf(ROOT, sizeof(ROOT), "%s", cands[i]);
      free(probe);
      return;
    }
    free(probe);
  }
  ROOT[0] = '\0';
  CHECK(0); // docs/cache-fingerprints.md not found from any candidate root
}

static char *root_path(const char *rel) {
  return ccb_path_join(ROOT, rel);
}

// ---------- doc sections ----------

typedef struct {
  char task[32];
  char variant[16];
  char lits[MAX_KEYS][32];
  size_t nlit;
  size_t nfile;
  char adhoc[MAX_KEYS][64];
  size_t nadhoc;
  int no_fp;
  int has_fence;
} doc_section;

static doc_section SECTIONS[MAX_SECTIONS];
static size_t NSECTIONS = 0;

static doc_section *find_section(const char *task, const char *variant) {
  for (size_t i = 0; i < NSECTIONS; i++) {
    if (strcmp(SECTIONS[i].task, task) == 0 &&
        strcmp(SECTIONS[i].variant, variant) == 0) {
      return &SECTIONS[i];
    }
  }
  return NULL;
}

// Parse one fp-block entry line into the current section.
static void parse_fp_entry(doc_section *s, char *line) {
  char *t = trim(line);
  if (t[0] == '\0') {
    return;
  }
  if (starts_with(t, "none:")) {
    s->no_fp = 1;
    return;
  }
  if (starts_with(t, "file:")) {
    s->nfile++;
    return;
  }
  if (starts_with(t, "adhoc:")) {
    char *v = trim(t + strlen("adhoc:"));
    CHECK(s->nadhoc < MAX_KEYS);
    if (s->nadhoc < MAX_KEYS) {
      snprintf(s->adhoc[s->nadhoc], sizeof(s->adhoc[0]), "%s", v);
      s->nadhoc++;
    }
    return;
  }
  // A bare literal key: one word, no spaces, no colon.
  CHECK(strchr(t, ' ') == NULL && strchr(t, ':') == NULL);
  CHECK(s->nlit < MAX_KEYS);
  if (s->nlit < MAX_KEYS) {
    snprintf(s->lits[s->nlit], sizeof(s->lits[0]), "%s", t);
    s->nlit++;
  }
}

// Parse the doc: task sections (### headings) with their fp fences,
// and the config-coverage table rows (kept in TABLE_* below).
static char TABLE_FIELDS[MAX_FIELDS][32];
static char TABLE_COVERAGE[MAX_FIELDS][256];
static size_t NTABLE = 0;

static void parse_doc(char *text) {
  line_list ll;
  split_lines(text, &ll);
  int in_fence = 0;
  int in_config = 0;
  doc_section *cur = NULL;
  for (size_t i = 0; i < ll.count; i++) {
    char *line = ll.lines[i];
    if (starts_with(line, "## Config coverage")) {
      in_config = 1;
      cur = NULL;
      continue;
    }
    if (starts_with(line, "## ") && !starts_with(line, "### ")) {
      in_config = 0;
      cur = NULL;
      continue;
    }
    if (in_config && line[0] == '|') {
      // Table row: | field | toml | coverage |.
      char *cols[4];
      size_t ncols = 0;
      char *p = line;
      while (*p != '\0' && ncols < 4) {
        if (*p == '|') {
          p++;
          char *start = p;
          while (*p != '\0' && *p != '|') {
            p++;
          }
          char save = *p;
          *p = '\0';
          cols[ncols++] = trim(start);
          *p = save;
        } else {
          p++;
        }
      }
      if (ncols >= 3) {
        if (strcmp(cols[0], "Field") == 0 || starts_with(cols[0], "---")) {
          continue;
        }
        CHECK(NTABLE < MAX_FIELDS);
        if (NTABLE < MAX_FIELDS) {
          snprintf(TABLE_FIELDS[NTABLE], sizeof(TABLE_FIELDS[0]), "%s", cols[0]);
          snprintf(TABLE_COVERAGE[NTABLE], sizeof(TABLE_COVERAGE[0]), "%s", cols[2]);
          NTABLE++;
        }
      }
      continue;
    }
    if (starts_with(line, "### ")) {
      char *t = trim(line + 4);
      CHECK(NSECTIONS < MAX_SECTIONS);
      if (NSECTIONS >= MAX_SECTIONS) {
        cur = NULL;
        continue;
      }
      cur = &SECTIONS[NSECTIONS++];
      memset(cur, 0, sizeof(*cur));
      first_word(t, cur->task, sizeof(cur->task));
      // Variant in parens, e.g. "native (clang)".
      char *lp = strchr(t, '(');
      char *rp = lp != NULL ? strchr(lp, ')') : NULL;
      if (lp != NULL && rp != NULL && (size_t)(rp - lp - 1) < sizeof(cur->variant)) {
        size_t n = (size_t)(rp - lp - 1);
        memcpy(cur->variant, lp + 1, n);
        cur->variant[n] = '\0';
      }
      in_fence = 0;
      continue;
    }
    if (cur != NULL && strcmp(trim(line), "```fp") == 0) {
      in_fence = 1;
      cur->has_fence = 1;
      continue;
    }
    if (in_fence && starts_with(trim(line), "```")) {
      in_fence = 0;
      continue;
    }
    if (in_fence && cur != NULL) {
      parse_fp_entry(cur, line);
    }
  }
  free(ll.lines);
}

// ---------- code fingerprint keys ----------

typedef struct {
  char task[32];
  char variant[16];
  char keys[MAX_KEYS][32];
  size_t nkeys;
  int has_file;
} code_entry;

static code_entry CODE[MAX_SECTIONS];
static size_t NCODE = 0;

typedef struct {
  const char *fn;
  const char *task;
  const char *task2; // shared helper: also attributed here
  const char *task3; // shared helper: and here
  const char *variant;
} fn_map;

static const fn_map FN_MAP[] = {
  { "ccb_step_compile_res", "compile-res", "", "", "" },
  { "ccb_step_link", "link", "", "", "" },
  { "ccb_step_code", "compile-code", "", "", "" },
  { "ccb_step_dex", "dex", "", "", "" },
  { "ccb_step_deps", "deps", "", "", "" },
  { "ccb_step_merge_manifest", "merge-manifest", "", "", "" },
  { "ccb_native_fingerprint", "native", "", "", "clang" },
  { "ccb_native_add_depfile", "native", "", "", "clang" },
  { "ccb_step_native", "native", "", "", "cmake" },
  { "ccb_mod_fingerprint", "module", "", "", "" },
  { "ccb_mod_fp_dep", "module", "", "", "" },
  { "ccb_mod_fp_files", "module", "", "", "" },
  { "ccb_pkg_fingerprint", "package", "", "", "" },
  { "ccb_fp_add_plugins", "compile-code", "module", "", "" },
  { "ccb_fp_add_aapt2_tools", "compile-res", "link", "module", "" },
  { "ccb_fp_add_kotlin_tools", "compile-code", "module", "", "" },
  { "ccb_stdlib_dex_key", "dex-stdlib", "", "", "" },
  { "ccb_classes_fingerprint", "dex", "", "", "" },
};

static code_entry *code_entry_for(const char *task, const char *variant) {
  for (size_t i = 0; i < NCODE; i++) {
    if (strcmp(CODE[i].task, task) == 0 &&
        strcmp(CODE[i].variant, variant) == 0) {
      return &CODE[i];
    }
  }
  CHECK(NCODE < MAX_SECTIONS);
  if (NCODE >= MAX_SECTIONS) {
    return NULL;
  }
  code_entry *e = &CODE[NCODE++];
  memset(e, 0, sizeof(*e));
  snprintf(e->task, sizeof(e->task), "%s", task);
  snprintf(e->variant, sizeof(e->variant), "%s", variant);
  return e;
}

static const fn_map *find_fn_map(const char *fn) {
  for (size_t i = 0; i < CCB_ARRAY_LEN(FN_MAP); i++) {
    if (strcmp(FN_MAP[i].fn, fn) == 0) {
      return &FN_MAP[i];
    }
  }
  return NULL;
}

static void record_literal(const char *fn, const char *key) {
  const fn_map *m = find_fn_map(fn);
  if (m == NULL) {
    fprintf(stderr, "untracked fingerprint key '%s' in function '%s'\n", key, fn);
    CHECK(0);
    return;
  }
  const char *tasks[3] = { m->task, m->task2, m->task3 };
  for (size_t t = 0; t < 3; t++) {
    if (tasks[t][0] == '\0') {
      continue;
    }
    code_entry *e = code_entry_for(tasks[t], m->variant);
    if (e == NULL) {
      continue;
    }
    int seen = 0;
    for (size_t i = 0; i < e->nkeys; i++) {
      if (strcmp(e->keys[i], key) == 0) {
        seen = 1;
      }
    }
    CHECK(e->nkeys < MAX_KEYS);
    if (!seen && e->nkeys < MAX_KEYS) {
      snprintf(e->keys[e->nkeys], sizeof(e->keys[0]), "%s", key);
      e->nkeys++;
    }
  }
}

static void record_file_call(const char *fn) {
  const fn_map *m = find_fn_map(fn);
  if (m == NULL) {
    return; // file calls in untracked functions are not fingerprint keys
  }
  const char *tasks[3] = { m->task, m->task2, m->task3 };
  for (size_t t = 0; t < 3; t++) {
    if (tasks[t][0] == '\0') {
      continue;
    }
    code_entry *e = code_entry_for(tasks[t], m->variant);
    if (e != NULL) {
      e->has_file = 1;
    }
  }
}

// Function name when a line starts a definition at column 0:
// no leading space, no '#', contains '(', no ';' on the line.
// The name is the identifier before the first '('.
static void def_fn_name(char *line, char *out, size_t n) {
  out[0] = '\0';
  if (line[0] == '\0' || is_space_c(line[0]) || line[0] == '#') {
    return;
  }
  char *lp = strchr(line, '(');
  if (lp == NULL || strchr(line, ';') != NULL) {
    return;
  }
  char *e = lp;
  while (e > line && is_ident_c(e[-1])) {
    e--;
  }
  size_t len = (size_t)(lp - e);
  if (len == 0 || len >= n) {
    return;
  }
  memcpy(out, e, len);
  out[len] = '\0';
}

// One line's ccb_fp_add* calls, attributed to the current function.
// A key add records its key only when the second argument is a
// string literal. A dynamic key is not a literal key: the merge step
// keys AAR manifest texts by AAR path, and the package step keys
// dex/asset/.so entries by archive name (with an empty value).
static void scan_line_fp(char *line, const char *fn) {
  char *p = line;
  while ((p = strstr(p, "ccb_fp_add")) != NULL) {
    char *q = p + strlen("ccb_fp_add");
    if (*q == 'l' && starts_with(q, "l(")) {
      q += 2;
    } else if (*q == '(') {
      q += 1;
    } else if (starts_with(q, "_file(")) {
      record_file_call(fn);
      p = q + 6;
      continue;
    } else {
      p = q;
      continue;
    }
    char *comma = strchr(q, ',');
    if (comma == NULL) {
      p = q;
      continue;
    }
    char *a = comma + 1;
    while (is_space_c(*a)) {
      a++;
    }
    if (*a == '"') {
      char *b = strchr(a + 1, '"');
      if (b != NULL && (size_t)(b - a - 1) < 32) {
        char key[32];
        size_t n = (size_t)(b - a - 1);
        memcpy(key, a + 1, n);
        key[n] = '\0';
        record_literal(fn, key);
      }
    }
    p = q;
  }
}

// ---------- cache step coverage ----------

static char STEP_TASKS[MAX_STEPS][32];
static size_t NSTEPS = 0;

static void note_step_task(const char *task) {
  for (size_t i = 0; i < NSTEPS; i++) {
    if (strcmp(STEP_TASKS[i], task) == 0) {
      return;
    }
  }
  CHECK(NSTEPS < MAX_STEPS);
  if (NSTEPS < MAX_STEPS) {
    snprintf(STEP_TASKS[NSTEPS], sizeof(STEP_TASKS[0]), "%s", task);
    NSTEPS++;
  }
}

// Map a cache step name to its doc task.
static void step_task_for(const char *step, char *out, size_t n) {
  if (starts_with(step, "link")) {
    snprintf(out, n, "link");
  } else if (starts_with(step, "sign")) {
    snprintf(out, n, "sign");
  } else if (starts_with(step, "package")) {
    snprintf(out, n, "package");
  } else if (starts_with(step, "verify")) {
    snprintf(out, n, "verify");
  } else if (starts_with(step, "module-") || starts_with(step, "library-")) {
    snprintf(out, n, "module");
  } else if (starts_with(step, "native-")) {
    snprintf(out, n, "native");
  } else {
    snprintf(out, n, "%s", step);
  }
}

// Resolve a ccb_cache_step_fp/set_step second argument: a string
// literal, or one of the step-key variables (link_key, sign_key,
// pkg_key, verify_key, key, step) to the steps it can name.
static void resolve_step_arg(char *arg) {
  while (is_space_c(*arg)) {
    arg++;
  }
  if (*arg == '"') {
    char *b = strchr(arg + 1, '"');
    if (b == NULL) {
      return;
    }
    char step[64];
    size_t n = (size_t)(b - arg - 1);
    if (n >= sizeof(step)) {
      return;
    }
    memcpy(step, arg + 1, n);
    step[n] = '\0';
    char task[32];
    step_task_for(step, task, sizeof(task));
    note_step_task(task);
    return;
  }
  char var[32];
  first_word(arg, var, sizeof(var));
  // first_word stops at spaces; the arg may end with ',' or ')'.
  size_t vl = strlen(var);
  while (vl > 0 && !is_ident_c(var[vl - 1])) {
    var[--vl] = '\0';
  }
  // The identifier may be qualified (c->pkg_key): keep the last one.
  char *last = var;
  for (char *p = var; *p != '\0'; p++) {
    if (!is_ident_c(*p)) {
      last = p + 1;
    }
  }
  if (strcmp(last, "link_key") == 0) {
    note_step_task("link");
  } else if (strcmp(last, "sign_key") == 0) {
    note_step_task("sign");
  } else if (strcmp(last, "pkg_key") == 0) {
    note_step_task("package");
  } else if (strcmp(last, "verify_key") == 0) {
    note_step_task("verify");
  } else if (strcmp(last, "key") == 0) {
    note_step_task("module");
  } else if (strcmp(last, "step") == 0) {
    note_step_task("native");
  } else {
    fprintf(stderr, "unknown cache step variable '%s'\n", last);
    CHECK(0);
  }
}

static void scan_line_steps(char *line) {
  const char *names[] = { "ccb_cache_step_fp(", "ccb_cache_set_step(" };
  for (size_t k = 0; k < CCB_ARRAY_LEN(names); k++) {
    char *p = strstr(line, names[k]);
    if (p == NULL) {
      continue;
    }
    char *comma = strchr(p, ',');
    if (comma == NULL) {
      continue;
    }
    resolve_step_arg(comma + 1);
  }
}

static const char *CODE_FILES[] = {
  "src/ccb_pipeline.c",
  "src/ccb_pipeline_code.c",
  "src/ccb_pipeline_deps.c",
  "src/ccb_pipeline_dex.c",
  "src/ccb_pipeline_module.c",
  "src/ccb_pipeline_modules.c",
  "src/ccb_pipeline_native.c",
  "src/ccb_pipeline_cmake.c",
  "src/ccb_pipeline_package.c",
  "src/ccb_pipeline_res.c",
  "src/ccb_pipeline_sign.c",
};

static void scan_code_file(const char *rel) {
  char *path = root_path(rel);
  char *text = ccb_test_try_read(path);
  free(path);
  CHECK(text != NULL);
  if (text == NULL) {
    return;
  }
  line_list ll;
  split_lines(text, &ll);
  char fn[64] = "";
  for (size_t i = 0; i < ll.count; i++) {
    char name[64];
    def_fn_name(ll.lines[i], name, sizeof(name));
    if (name[0] != '\0') {
      snprintf(fn, sizeof(fn), "%s", name);
    }
    scan_line_fp(ll.lines[i], fn);
    scan_line_steps(ll.lines[i]);
  }
  free(ll.lines);
  free(text);
}

// ---------- config struct fields ----------

// Last identifier token in s (before the first ';'), or 0.
static int last_ident(char *s, char *out, size_t n) {
  char *last = NULL;
  size_t lastlen = 0;
  for (char *p = s; *p != '\0';) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
      char *t = p;
      while (is_ident_c(*p)) {
        p++;
      }
      last = t;
      lastlen = (size_t)(p - t);
    } else {
      p++;
    }
  }
  if (last == NULL || lastlen >= n) {
    return 0;
  }
  memcpy(out, last, lastlen);
  out[lastlen] = '\0';
  return 1;
}

static int ends_with_count(const char *s) {
  size_t n = strlen(s);
  return n > 6 && strcmp(s + n - 6, "_count") == 0;
}

// Fields of one struct in ccb_config.h, between its "typedef struct {"
// and the end marker ("} ccb_module;" / "} ccb_config;"). Length
// (*_count) fields are not config keys and are skipped.
static size_t parse_struct_fields(char *text, const char *end_marker,
    char fields[][32]) {
  char *end = strstr(text, end_marker);
  CHECK(end != NULL);
  if (end == NULL) {
    return 0;
  }
  char *start = NULL;
  char *p = text;
  while ((p = strstr(p, "typedef struct {")) != NULL && p < end) {
    start = p;
    p += strlen("typedef struct {");
  }
  CHECK(start != NULL);
  if (start == NULL) {
    return 0;
  }
  size_t len = (size_t)(end - start);
  char *range = ccb_xmalloc(len + 1);
  memcpy(range, start, len);
  range[len] = '\0';
  line_list ll;
  split_lines(range, &ll);
  size_t n = 0;
  for (size_t i = 0; i < ll.count; i++) {
    char *line = ll.lines[i];
    char *hash = strstr(line, "//");
    if (hash != NULL) {
      *hash = '\0';
    }
    char *semi = strchr(line, ';');
    if (semi == NULL) {
      continue;
    }
    *semi = '\0';
    char ident[32];
    if (!last_ident(line, ident, sizeof(ident))) {
      continue;
    }
    if (ends_with_count(ident)) {
      continue;
    }
    if (strcmp(ident, "typedef") == 0 || strcmp(ident, "struct") == 0) {
      continue;
    }
    CHECK(n < MAX_FIELDS);
    if (n < MAX_FIELDS) {
      snprintf(fields[n], sizeof(fields[0]), "%s", ident);
      n++;
    }
  }
  free(ll.lines);
  free(range);
  return n;
}

// ---------- checks ----------

static const char *KNOWN_TASKS[] = {
  "deps", "merge-manifest", "compile-res", "link", "compile-code",
  "dex", "dexcheck", "native", "dex-stdlib", "module", "package",
  "align", "sign", "verify",
};

static const struct {
  const char *task;
  const char *variant;
} REQUIRED[] = {
  { "deps", "" }, { "merge-manifest", "" }, { "compile-res", "" },
  { "link", "" }, { "compile-code", "" }, { "dex", "" },
  { "dexcheck", "" }, { "native", "clang" }, { "native", "cmake" },
  { "dex-stdlib", "" }, { "module", "" }, { "package", "" },
  { "align", "" }, { "sign", "" }, { "verify", "" },
};

static int is_known_task(const char *t) {
  for (size_t i = 0; i < CCB_ARRAY_LEN(KNOWN_TASKS); i++) {
    if (strcmp(KNOWN_TASKS[i], t) == 0) {
      return 1;
    }
  }
  return 0;
}

static int set_contains(char words[][32], size_t n, const char *w) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(words[i], w) == 0) {
      return 1;
    }
  }
  return 0;
}

static void check_sections(void) {
  for (size_t i = 0; i < NSECTIONS; i++) {
    CHECK(is_known_task(SECTIONS[i].task));
    CHECK(SECTIONS[i].has_fence);
  }
  for (size_t i = 0; i < CCB_ARRAY_LEN(REQUIRED); i++) {
    CHECK(find_section(REQUIRED[i].task, REQUIRED[i].variant) != NULL);
  }
}

static void check_literal_sets(void) {
  for (size_t i = 0; i < NCODE; i++) {
    doc_section *doc = find_section(CODE[i].task, CODE[i].variant);
    CHECK(doc != NULL);
    if (doc == NULL) {
      continue;
    }
    CHECK(!doc->no_fp);
    for (size_t k = 0; k < CODE[i].nkeys; k++) {
      if (!set_contains(doc->lits, doc->nlit, CODE[i].keys[k])) {
        fprintf(stderr, "doc is missing fingerprint key '%s' for task '%s%s%s'\n",
          CODE[i].keys[k], CODE[i].task,
          CODE[i].variant[0] != '\0' ? " (" : "",
          CODE[i].variant);
      }
      CHECK(set_contains(doc->lits, doc->nlit, CODE[i].keys[k]));
    }
    for (size_t k = 0; k < doc->nlit; k++) {
      if (!set_contains(CODE[i].keys, CODE[i].nkeys, doc->lits[k])) {
        fprintf(stderr, "doc lists stale fingerprint key '%s' for task '%s%s%s'\n",
          doc->lits[k], CODE[i].task,
          CODE[i].variant[0] != '\0' ? " (" : "",
          CODE[i].variant);
      }
      CHECK(set_contains(CODE[i].keys, CODE[i].nkeys, doc->lits[k]));
    }
    if (CODE[i].has_file) {
      CHECK(doc->nfile >= 1);
    }
  }
  // Every documented (non-adhoc, non-none) section must have a code
  // entry behind it, so a doc row cannot survive its code.
  for (size_t i = 0; i < NSECTIONS; i++) {
    if (SECTIONS[i].no_fp || SECTIONS[i].nadhoc > 0) {
      continue;
    }
    int found = 0;
    for (size_t k = 0; k < NCODE; k++) {
      if (strcmp(CODE[k].task, SECTIONS[i].task) == 0 &&
          strcmp(CODE[k].variant, SECTIONS[i].variant) == 0) {
        found = 1;
      }
    }
    CHECK(found);
  }
}

static void check_adhoc_list(doc_section *doc, const char **want, size_t nwant) {
  CHECK(doc != NULL);
  if (doc == NULL) {
    return;
  }
  CHECK(doc->nlit == 0);
  CHECK(doc->nadhoc == nwant);
  for (size_t i = 0; i < nwant && i < doc->nadhoc; i++) {
    CHECK_STR_EQ(doc->adhoc[i], want[i]);
  }
}

static int region_contains(char *a, char *b, const char *needle) {
  size_t nlen = strlen(needle);
  for (char *p = a; p + nlen <= b; p++) {
    if (strncmp(p, needle, nlen) == 0) {
      return 1;
    }
  }
  return 0;
}

// The ad-hoc fingerprints (align/sign/verify) are built by string
// concatenation, so the doc lists their components and this checks
// both sides: the doc lists against the expected components, and the
// code regions that build them mention the components. The sign
// check is the regression pin for the Step 3 alias fix.
static void check_adhoc(void) {
  static const char *align_want[] = { "zipalign path", "unsigned APK hash" };
  static const char *sign_want[] = { "variant", "keystore path",
    "input APK hash", "keystore hash (when the keystore exists)",
    "keystore alias" };
  static const char *verify_want[] = { "signed artifact hash" };
  check_adhoc_list(find_section("align", ""), align_want,
    CCB_ARRAY_LEN(align_want));
  check_adhoc_list(find_section("sign", ""), sign_want,
    CCB_ARRAY_LEN(sign_want));
  check_adhoc_list(find_section("verify", ""), verify_want,
    CCB_ARRAY_LEN(verify_want));
  char *path = root_path("src/ccb_pipeline_sign.c");
  char *text = ccb_test_try_read(path);
  free(path);
  CHECK(text != NULL);
  if (text == NULL) {
    return;
  }
  char *a = strstr(text, "ccb_str_init(&fpbuf);");
  char *b = a != NULL ? strstr(a, "ccb_str_steal(&fpbuf);") : NULL;
  CHECK(a != NULL && b != NULL && b > a);
  if (a != NULL && b != NULL) {
    CHECK(region_contains(a, b, "alias"));
    CHECK(region_contains(a, b, "keystore"));
    CHECK(region_contains(a, b, "hash"));
  }
  a = strstr(text, "ccb_str_init(&afp);");
  b = a != NULL ? strstr(a, "ccb_str_steal(&afp);") : NULL;
  CHECK(a != NULL && b != NULL && b > a);
  if (a != NULL && b != NULL) {
    CHECK(region_contains(a, b, "zipalign"));
    CHECK(region_contains(a, b, "hash"));
  }
  CHECK(strstr(text, "ccb_cache_hash_file(apk_path)") != NULL);
  free(text);
}

// The global tool fingerprint's components, in ccb_sdk.c
// (ccb_sdk_toolchain_identity, the single definition).
static void check_tool_fp(void) {
  static const char *markers[] = { "\"bt=\"", "\"|aapt2=\"", "\"|zipalign=\"",
    "\"|kt=\"", "\"|java=", "\"|compile=" };
  char *path = root_path("src/ccb_sdk.c");
  char *text = ccb_test_try_read(path);
  free(path);
  CHECK(text != NULL);
  if (text == NULL) {
    return;
  }
  for (size_t i = 0; i < CCB_ARRAY_LEN(markers); i++) {
    CHECK(strstr(text, markers[i]) != NULL);
  }
  free(text);
}

static void check_steps(void) {
  for (size_t i = 0; i < NSTEPS; i++) {
    CHECK(is_known_task(STEP_TASKS[i]));
  }
}

// First word of a coverage segment: letters, digits, '-'.
// Returns 0 when there is no word.
static int coverage_word(const char *s, char *out, size_t n) {
  while (is_space_c(*s)) {
    s++;
  }
  size_t i = 0;
  while ((is_ident_c(s[i]) || s[i] == '-') && i + 1 < n) {
    out[i] = s[i];
    i++;
  }
  out[i] = '\0';
  return i > 0;
}

static int is_coverage_word(const char *w) {
  if (is_known_task(w)) {
    return 1;
  }
  return strcmp(w, "tool") == 0 || strcmp(w, "indirect") == 0 ||
    strcmp(w, "none") == 0 || strcmp(w, "output-check") == 0;
}

static void check_config_table(char fields[][32], size_t nfields,
    char mfields[][32], size_t nmfields) {
  // Every struct field must have at least as many table rows as the
  // number of structs that declare it (shared names like `name`
  // need one row per struct).
  for (size_t i = 0; i < nfields; i++) {
    size_t need = 1;
    for (size_t k = 0; k < nmfields; k++) {
      if (strcmp(mfields[k], fields[i]) == 0) {
        need = 2;
      }
    }
    size_t have = 0;
    for (size_t k = 0; k < NTABLE; k++) {
      if (strcmp(TABLE_FIELDS[k], fields[i]) == 0) {
        have++;
      }
    }
    CHECK(have >= need);
  }
  for (size_t i = 0; i < nmfields; i++) {
    size_t have = 0;
    for (size_t k = 0; k < NTABLE; k++) {
      if (strcmp(TABLE_FIELDS[k], mfields[i]) == 0) {
        have++;
      }
    }
    CHECK(have >= 1);
  }
  // Every table row names a real field, has a coverage, and every
  // coverage segment starts with a known task or keyword.
  for (size_t i = 0; i < NTABLE; i++) {
    CHECK(TABLE_FIELDS[i][0] != '\0');
    int known = 0;
    for (size_t k = 0; k < nfields; k++) {
      if (strcmp(TABLE_FIELDS[i], fields[k]) == 0) {
        known = 1;
      }
    }
    for (size_t k = 0; k < nmfields; k++) {
      if (strcmp(TABLE_FIELDS[i], mfields[k]) == 0) {
        known = 1;
      }
    }
    CHECK(known);
    CHECK(TABLE_COVERAGE[i][0] != '\0');
    // Coverage is a list of items separated by ',' or ';'. An item
    // is a task id or keyword, optionally followed by ": prose" that
    // runs to the end — so once a segment contains ':', the rest is
    // prose (which may itself contain commas and semicolons) and is
    // no longer checked as items.
    char *cov = TABLE_COVERAGE[i];
    char *seg = cov;
    int in_prose = 0;
    while (seg != NULL && !in_prose) {
      char *next = strchr(seg, ',');
      char *semi = strchr(seg, ';');
      if (semi != NULL && (next == NULL || semi < next)) {
        next = semi;
      }
      char save = '\0';
      if (next != NULL) {
        save = *next;
        *next = '\0';
      }
      char word[32];
      if (coverage_word(seg, word, sizeof(word))) {
        CHECK(is_coverage_word(word));
      }
      if (strchr(seg, ':') != NULL) {
        in_prose = 1;
      }
      if (next != NULL) {
        *next = save;
        seg = next + 1;
      } else {
        seg = NULL;
      }
    }
  }
}

int main(void) {
  find_repo_root();
  CHECK(ROOT[0] != '\0');
  if (ROOT[0] == '\0') {
    TEST_MAIN_END();
  }
  char *docpath = root_path("docs/cache-fingerprints.md");
  char *doctext = ccb_test_try_read(docpath);
  free(docpath);
  CHECK(doctext != NULL);
  if (doctext == NULL) {
    TEST_MAIN_END();
  }
  parse_doc(doctext);
  free(doctext);
  check_sections();
  for (size_t i = 0; i < CCB_ARRAY_LEN(CODE_FILES); i++) {
    scan_code_file(CODE_FILES[i]);
  }
  check_literal_sets();
  check_adhoc();
  check_tool_fp();
  check_steps();
  char *cfgpath = root_path("src/ccb_config.h");
  char *cfgtext = ccb_test_try_read(cfgpath);
  free(cfgpath);
  CHECK(cfgtext != NULL);
  if (cfgtext == NULL) {
    TEST_MAIN_END();
  }
  static char fields[MAX_FIELDS][32];
  static char mfields[MAX_FIELDS][32];
  size_t nfields = parse_struct_fields(cfgtext, "} ccb_config;", fields);
  size_t nmfields = parse_struct_fields(cfgtext, "} ccb_module;", mfields);
  free(cfgtext);
  CHECK(nfields > 40 && nmfields > 5);
  check_config_table(fields, nfields, mfields, nmfields);
  TEST_MAIN_END();
}
