// ccb_manifest implementation: subset XML parser + AAR manifest merger.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_manifest.h"

// ---------- cursor parser ----------

typedef struct {
  const char *s;
  size_t len;
  size_t pos;
  char *err; // owned; set on first failure
} cur;

static int cur_fail(cur *c, const char *msg) {
  if (c->err == NULL) {
    c->err = ccb_xstrdup(msg);
  }
  return -1;
}

static int is_ws(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static int is_name_start(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == ':';
}

static int is_name_char(char ch) {
  return is_name_start(ch) || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
}

static void skip_ws(cur *c) {
  while (c->pos < c->len && is_ws(c->s[c->pos])) {
    c->pos++;
  }
}

static int starts_with(cur *c, const char *word) {
  size_t n = strlen(word);
  return c->pos + n <= c->len && memcmp(c->s + c->pos, word, n) == 0;
}

// Skip a comment, processing instruction, or <!...> declaration starting
// at the cursor (which must be at '<'). A DOCTYPE with an internal
// subset is skipped through "]>".
static int skip_markup(cur *c) {
  if (starts_with(c, "<!--")) {
    c->pos += 4;
    while (c->pos + 2 < c->len && !starts_with(c, "-->")) {
      c->pos++;
    }
    if (c->pos + 2 >= c->len) {
      return cur_fail(c, "unterminated comment");
    }
    c->pos += 3;
    return 0;
  }
  if (starts_with(c, "<?")) {
    c->pos += 2;
    while (c->pos + 1 < c->len && !starts_with(c, "?>")) {
      c->pos++;
    }
    if (c->pos + 1 >= c->len) {
      return cur_fail(c, "unterminated processing instruction");
    }
    c->pos += 2;
    return 0;
  }
  if (starts_with(c, "<!")) {
    c->pos += 2;
    int bracket = 0;
    while (c->pos < c->len) {
      if (c->s[c->pos] == '[') {
        bracket = 1;
      } else if (bracket && starts_with(c, "]>")) {
        c->pos += 2;
        return 0;
      } else if (!bracket && c->s[c->pos] == '>') {
        c->pos++;
        return 0;
      }
      c->pos++;
    }
    return cur_fail(c, "unterminated declaration");
  }
  return cur_fail(c, "expected markup");
}

// Skip whitespace and inter-element markup (comments, PIs, declarations).
static int skip_misc(cur *c) {
  for (;;) {
    skip_ws(c);
    if (c->pos < c->len && c->s[c->pos] == '<' && c->pos + 1 < c->len &&
        (c->s[c->pos + 1] == '!' || c->s[c->pos + 1] == '?')) {
      if (skip_markup(c) != 0) {
        return -1;
      }
      continue;
    }
    return 0;
  }
}

// Local name: the part after the last ':', or the whole name.
static const char *local_name(const char *name) {
  const char *colon = strrchr(name, ':');
  return colon != NULL ? colon + 1 : name;
}

typedef struct {
  char *name; // owned
  char *value; // owned
  size_t vstart; // absolute offset of the value text (inside quotes)
  size_t vend;
} attr;

typedef struct elem {
  char *name; // local name, owned
  attr *attrs;
  size_t nattrs;
  size_t capattrs;
  struct elem **kids;
  size_t nkids;
  size_t capkids;
  size_t start; // '<' offset
  size_t end; // offset just past the close tag (or '/>')
  size_t close_start; // offset of '</name>', SIZE_MAX when self-closing
  size_t inner_start;
  size_t inner_end;
} elem;

static void elem_free(elem *e) {
  if (e == NULL) {
    return;
  }
  free(e->name);
  for (size_t i = 0; i < e->nattrs; i++) {
    free(e->attrs[i].name);
    free(e->attrs[i].value);
  }
  free(e->attrs);
  for (size_t i = 0; i < e->nkids; i++) {
    elem_free(e->kids[i]);
  }
  free(e->kids);
  free(e);
}

static const attr *elem_attr(const elem *e, const char *name) {
  for (size_t i = 0; i < e->nattrs; i++) {
    if (strcmp(e->attrs[i].name, name) == 0) {
      return &e->attrs[i];
    }
  }
  return NULL;
}

// Parse one element starting at '<'. Children are parsed recursively so
// the result is a small tree for exactly the levels the merger inspects;
// deeper content is validated but otherwise untouched.
static int parse_element(cur *c, elem **out) {
  elem *e = ccb_xcalloc(1, sizeof(*e));
  e->close_start = SIZE_MAX;
  e->start = c->pos;
  c->pos++; // '<'
  size_t nstart = c->pos;
  while (c->pos < c->len && is_name_char(c->s[c->pos])) {
    c->pos++;
  }
  if (c->pos == nstart) {
    elem_free(e);
    return cur_fail(c, "expected element name");
  }
  char *raw_name = ccb_xstrndup(c->s + nstart, c->pos - nstart);
  e->name = ccb_xstrdup(local_name(raw_name));
  free(raw_name);
  // Attributes.
  for (;;) {
    skip_ws(c);
    if (c->pos >= c->len) {
      elem_free(e);
      return cur_fail(c, "unexpected end of input inside element");
    }
    if (starts_with(c, "/>")) {
      c->pos += 2;
      e->end = c->pos;
      e->inner_start = e->end;
      e->inner_end = e->end;
      *out = e;
      return 0;
    }
    if (c->s[c->pos] == '>') {
      c->pos++;
      break;
    }
    size_t astart = c->pos;
    while (c->pos < c->len && is_name_char(c->s[c->pos])) {
      c->pos++;
    }
    if (c->pos == astart) {
      elem_free(e);
      return cur_fail(c, "expected attribute name");
    }
    char *aname = ccb_xstrndup(c->s + astart, c->pos - astart);
    if (elem_attr(e, aname) != NULL) {
      free(aname);
      elem_free(e);
      return cur_fail(c, "duplicate attribute");
    }
    skip_ws(c);
    if (c->pos >= c->len || c->s[c->pos] != '=') {
      free(aname);
      elem_free(e);
      return cur_fail(c, "expected '=' after attribute name");
    }
    c->pos++;
    skip_ws(c);
    if (c->pos >= c->len || (c->s[c->pos] != '"' && c->s[c->pos] != '\'')) {
      free(aname);
      elem_free(e);
      return cur_fail(c, "expected quoted attribute value");
    }
    char quote = c->s[c->pos];
    c->pos++;
    size_t vstart = c->pos;
    while (c->pos < c->len && c->s[c->pos] != quote) {
      c->pos++;
    }
    if (c->pos >= c->len) {
      free(aname);
      elem_free(e);
      return cur_fail(c, "unterminated attribute value");
    }
    size_t vend = c->pos;
    c->pos++; // closing quote
    if (e->nattrs == e->capattrs) {
      size_t ncap = e->capattrs == 0 ? 8 : e->capattrs * 2;
      e->attrs = ccb_xrealloc(e->attrs, ncap * sizeof(*e->attrs));
      e->capattrs = ncap;
    }
    e->attrs[e->nattrs].name = aname;
    e->attrs[e->nattrs].value = ccb_xstrndup(c->s + vstart, vend - vstart);
    e->attrs[e->nattrs].vstart = vstart;
    e->attrs[e->nattrs].vend = vend;
    e->nattrs++;
  }
  e->inner_start = c->pos;
  // Children and text until the matching close tag.
  for (;;) {
    if (c->pos >= c->len) {
      elem_free(e);
      return cur_fail(c, "unclosed element");
    }
    if (starts_with(c, "</")) {
      size_t cs = c->pos;
      c->pos += 2;
      size_t cstart = c->pos;
      while (c->pos < c->len && is_name_char(c->s[c->pos])) {
        c->pos++;
      }
      if (c->pos == cstart) {
        elem_free(e);
        return cur_fail(c, "expected close tag name");
      }
      char *cname = ccb_xstrndup(c->s + cstart, c->pos - cstart);
      int match = strcmp(local_name(cname), e->name) == 0;
      free(cname);
      if (!match) {
        elem_free(e);
        return cur_fail(c, "mismatched close tag");
      }
      skip_ws(c);
      if (c->pos >= c->len || c->s[c->pos] != '>') {
        elem_free(e);
        return cur_fail(c, "expected '>' after close tag");
      }
      c->pos++;
      e->close_start = cs;
      e->inner_end = cs;
      e->end = c->pos;
      *out = e;
      return 0;
    }
    if (c->s[c->pos] == '<') {
      if (c->pos + 1 < c->len && (c->s[c->pos + 1] == '!' || c->s[c->pos + 1] == '?')) {
        if (starts_with(c, "<![CDATA[")) {
          c->pos += 9;
          while (c->pos + 2 < c->len && !starts_with(c, "]]>")) {
            c->pos++;
          }
          if (c->pos + 2 >= c->len) {
            elem_free(e);
            return cur_fail(c, "unterminated CDATA section");
          }
          c->pos += 3;
          continue;
        }
        if (skip_markup(c) != 0) {
          elem_free(e);
          return -1;
        }
        continue;
      }
      elem *kid = NULL;
      if (parse_element(c, &kid) != 0) {
        elem_free(e);
        return -1;
      }
      if (e->nkids == e->capkids) {
        size_t ncap = e->capkids == 0 ? 8 : e->capkids * 2;
        e->kids = ccb_xrealloc(e->kids, ncap * sizeof(*e->kids));
        e->capkids = ncap;
      }
      e->kids[e->nkids++] = kid;
      continue;
    }
    c->pos++; // text content: skipped, kept verbatim via spans
  }
}

// ---------- manifest model ----------

typedef struct {
  char *kind; // "activity", "uses-permission", ...
  char *raw; // raw android:name, or NULL for a nameless uses-feature
  char *resolved; // package-resolved name, or NULL like raw
  char *text; // full element text from the source manifest
  char *norm; // normalized comparison key
  size_t vstart; // android:name value offset within text, SIZE_MAX if none
  size_t vend;
} entry;

typedef struct {
  char *package; // owned or NULL
  entry *entries;
  size_t nentries;
  size_t capentries;
  size_t uses_insert; // offset for uses-* entries, SIZE_MAX if none
  size_t comp_insert; // offset for component entries, SIZE_MAX if none
} manifest;

static void entry_free_contents(entry *e) {
  free(e->kind);
  free(e->raw);
  free(e->resolved);
  free(e->text);
  free(e->norm);
}

static void manifest_free(manifest *m) {
  if (m == NULL) {
    return;
  }
  free(m->package);
  for (size_t i = 0; i < m->nentries; i++) {
    entry_free_contents(&m->entries[i]);
  }
  free(m->entries);
  free(m);
}

static int is_component_kind(const char *name) {
  return strcmp(name, "activity") == 0 || strcmp(name, "service") == 0 ||
      strcmp(name, "receiver") == 0 || strcmp(name, "provider") == 0 ||
      strcmp(name, "activity-alias") == 0;
}

// Resolve a component name against a package: ".Foo" -> pkg + ".Foo",
// "Foo" -> pkg + ".Foo", anything else unchanged. A NULL package leaves
// the name as written.
static char *resolve_name(const char *name, const char *pkg) {
  if (pkg == NULL || name[0] == '\0') {
    return ccb_xstrdup(name);
  }
  if (name[0] == '.') {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, pkg);
    ccb_str_append(&s, name);
    return ccb_str_steal(&s);
  }
  if (strchr(name, '.') == NULL) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, pkg);
    ccb_str_append_c(&s, '.');
    ccb_str_append(&s, name);
    return ccb_str_steal(&s);
  }
  return ccb_xstrdup(name);
}

// Normalized comparison key: kind + resolved name + attributes sorted by
// name + inner XML with whitespace runs collapsed. Two entries with the
// same key are the same declaration, formatting aside.
static char *norm_key(const char *kind, const char *resolved, const elem *e,
    const char *src) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, kind);
  ccb_str_append_c(&s, '\n');
  if (resolved != NULL) {
    ccb_str_append(&s, resolved);
  }
  ccb_str_append_c(&s, '\n');
  // Insertion sort of attribute indices by name (attribute counts are tiny).
  size_t *idx = NULL;
  if (e->nattrs > 0) {
    idx = ccb_xmalloc(e->nattrs * sizeof(*idx));
    for (size_t i = 0; i < e->nattrs; i++) {
      idx[i] = i;
    }
    for (size_t i = 1; i < e->nattrs; i++) {
      size_t j = i;
      while (j > 0 && strcmp(e->attrs[idx[j - 1]].name, e->attrs[idx[j]].name) > 0) {
        size_t tmp = idx[j - 1];
        idx[j - 1] = idx[j];
        idx[j] = tmp;
        j--;
      }
    }
    for (size_t i = 0; i < e->nattrs; i++) {
      const attr *a = &e->attrs[idx[i]];
      ccb_str_append(&s, a->name);
      ccb_str_append(&s, "=\"");
      // android:name compares by resolved form: ".Shared" in one AAR
      // and "com.example.lib.Shared" in another are the same entry.
      if (resolved != NULL && strcmp(a->name, "android:name") == 0) {
        ccb_str_append(&s, resolved);
      } else {
        ccb_str_append(&s, a->value);
      }
      ccb_str_append(&s, "\" ");
    }
    free(idx);
  }
  ccb_str_append_c(&s, '\n');
  int pending_space = 0;
  int seen = 0;
  for (size_t i = e->inner_start; i < e->inner_end; i++) {
    char ch = src[i];
    if (is_ws(ch)) {
      if (seen) {
        pending_space = 1;
      }
      continue;
    }
    if (pending_space) {
      ccb_str_append_c(&s, ' ');
      pending_space = 0;
    }
    ccb_str_append_c(&s, ch);
    seen = 1;
  }
  return ccb_str_steal(&s);
}

static int manifest_push_entry(manifest *m, const elem *e, const char *src,
    const char *pkg, const char *kind, const char *raw, cur *c) {
  if (m->nentries == m->capentries) {
    size_t ncap = m->capentries == 0 ? 8 : m->capentries * 2;
    m->entries = ccb_xrealloc(m->entries, ncap * sizeof(*m->entries));
    m->capentries = ncap;
  }
  entry *en = &m->entries[m->nentries];
  memset(en, 0, sizeof(*en));
  en->kind = ccb_xstrdup(kind);
  en->raw = raw != NULL ? ccb_xstrdup(raw) : NULL;
  en->resolved = raw != NULL ? resolve_name(raw, pkg) : NULL;
  en->text = ccb_xstrndup(src + e->start, e->end - e->start);
  en->norm = norm_key(kind, en->resolved, e, src);
  const attr *name_attr = elem_attr(e, "android:name");
  if (name_attr != NULL) {
    en->vstart = name_attr->vstart - e->start;
    en->vend = name_attr->vend - e->start;
  } else {
    en->vstart = SIZE_MAX;
    en->vend = SIZE_MAX;
  }
  m->nentries++;
  (void)c;
  return 0;
}

// Parse one manifest document. fallback_pkg resolves relative names when
// the manifest itself declares no package (the app package, for AARs).
// Returns NULL with *err set (owned) on failure.
static manifest *parse_manifest(const char *text, const char *fallback_pkg,
    char **err) {
  cur c;
  memset(&c, 0, sizeof(c));
  c.s = text;
  c.len = strlen(text);
  c.pos = 0;
  c.err = NULL;
  // Skip a UTF-8 byte order mark.
  if (c.len >= 3 && (unsigned char)c.s[0] == 0xef && (unsigned char)c.s[1] == 0xbb &&
      (unsigned char)c.s[2] == 0xbf) {
    c.pos = 3;
  }
  manifest *m = ccb_xcalloc(1, sizeof(*m));
  m->uses_insert = SIZE_MAX;
  m->comp_insert = SIZE_MAX;
  if (skip_misc(&c) != 0) {
    goto fail;
  }
  if (c.pos >= c.len || c.s[c.pos] != '<') {
    cur_fail(&c, "no root element");
    goto fail;
  }
  elem *root = NULL;
  if (parse_element(&c, &root) != 0) {
    goto fail;
  }
  if (strcmp(root->name, "manifest") != 0) {
    cur_fail(&c, "root element is not <manifest>");
    elem_free(root);
    goto fail;
  }
  const attr *pkg_attr = elem_attr(root, "package");
  if (pkg_attr != NULL) {
    m->package = ccb_xstrdup(pkg_attr->value);
  }
  const char *resolve_pkg = m->package != NULL ? m->package : fallback_pkg;
  elem *app = NULL;
  for (size_t i = 0; i < root->nkids; i++) {
    elem *kid = root->kids[i];
    if (strcmp(kid->name, "application") == 0) {
      if (app != NULL) {
        cur_fail(&c, "multiple <application> elements");
        elem_free(root);
        goto fail;
      }
      app = kid;
    }
  }
  if (app != NULL) {
    m->uses_insert = app->start;
    if (app->close_start != SIZE_MAX) {
      m->comp_insert = app->close_start;
    }
    for (size_t i = 0; i < app->nkids; i++) {
      elem *kid = app->kids[i];
      if (!is_component_kind(kid->name)) {
        continue; // application attributes and meta-data are app-owned
      }
      const attr *name_attr = elem_attr(kid, "android:name");
      if (name_attr == NULL) {
        cur_fail(&c, "component without android:name");
        elem_free(root);
        goto fail;
      }
      if (manifest_push_entry(m, kid, text, resolve_pkg, kid->name, name_attr->value, &c) != 0) {
        elem_free(root);
        goto fail;
      }
    }
  } else if (root->close_start != SIZE_MAX) {
    m->uses_insert = root->close_start;
  }
  for (size_t i = 0; i < root->nkids; i++) {
    elem *kid = root->kids[i];
    if (strcmp(kid->name, "uses-permission") == 0) {
      const attr *name_attr = elem_attr(kid, "android:name");
      if (name_attr == NULL) {
        cur_fail(&c, "uses-permission without android:name");
        elem_free(root);
        goto fail;
      }
      if (manifest_push_entry(m, kid, text, resolve_pkg, kid->name, name_attr->value, &c) != 0) {
        elem_free(root);
        goto fail;
      }
    } else if (strcmp(kid->name, "uses-feature") == 0) {
      const attr *name_attr = elem_attr(kid, "android:name");
      const char *raw = name_attr != NULL ? name_attr->value : NULL;
      if (manifest_push_entry(m, kid, text, resolve_pkg, kid->name, raw, &c) != 0) {
        elem_free(root);
        goto fail;
      }
    }
    // Any other top-level element (uses-sdk, permission declarations,
    // queries, ...) is not merged; the app manifest owns it.
  }
  elem_free(root);
  if (skip_misc(&c) != 0) {
    goto fail;
  }
  if (c.pos != c.len) {
    cur_fail(&c, "trailing content after </manifest>");
    goto fail;
  }
  return m;
fail:
  if (err != NULL && *err == NULL) {
    *err = c.err;
    c.err = NULL;
  }
  free(c.err);
  manifest_free(m);
  return NULL;
}

static void warns_push(ccb_manifest_warnings *warns, const char *msg) {
  if (warns == NULL) {
    return;
  }
  if (warns->count == warns->cap) {
    size_t ncap = warns->cap == 0 ? 8 : warns->cap * 2;
    warns->msgs = ccb_xrealloc(warns->msgs, ncap * sizeof(*warns->msgs));
    warns->cap = ncap;
  }
  warns->msgs[warns->count++] = ccb_xstrdup(msg);
}

void ccb_manifest_warnings_free(ccb_manifest_warnings *warns) {
  if (warns == NULL) {
    return;
  }
  for (size_t i = 0; i < warns->count; i++) {
    free(warns->msgs[i]);
  }
  free(warns->msgs);
  warns->msgs = NULL;
  warns->count = 0;
  warns->cap = 0;
}

// Identity match for conflict detection: same kind and, for named
// entries, the same resolved name. Nameless uses-features have no name
// to conflict on; they only deduplicate on the full normalized key.
static int entry_same_identity(const entry *a, const entry *b) {
  if (strcmp(a->kind, b->kind) != 0) {
    return 0;
  }
  if (a->resolved != NULL && b->resolved != NULL) {
    return strcmp(a->resolved, b->resolved) == 0;
  }
  return 0;
}

// Emit an accepted entry's text, rewriting android:name to the resolved
// name when the raw form was relative.
static void emit_entry(ccb_str *out, const entry *e) {
  ccb_str_append(out, "    ");
  if (e->raw != NULL && e->resolved != NULL && strcmp(e->raw, e->resolved) != 0 &&
      e->vstart != SIZE_MAX) {
    ccb_str_append_n(out, e->text, e->vstart);
    ccb_str_append(out, e->resolved);
    ccb_str_append(out, e->text + e->vend);
  } else {
    ccb_str_append(out, e->text);
  }
  ccb_str_append_c(out, '\n');
}

char *ccb_manifest_merge(const char *app_text,
    const char **aar_texts, const char **aar_names, size_t aar_count,
    ccb_manifest_warnings *warns, char **err_out) {
  if (err_out != NULL) {
    *err_out = NULL;
  }
  char *app_err = NULL;
  manifest *app = parse_manifest(app_text, NULL, &app_err);
  if (app == NULL) {
    if (err_out != NULL) {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "app manifest: ");
      ccb_str_append(&s, app_err != NULL ? app_err : "parse error");
      *err_out = ccb_str_steal(&s);
    }
    free(app_err);
    return NULL;
  }
  // Accepted entries, in merge order, split by insertion point.
  entry **uses = NULL;
  size_t nuses = 0;
  size_t capuses = 0;
  entry **comps = NULL;
  size_t ncomps = 0;
  size_t capcomps = 0;
  // First-wins tracking for AAR-vs-AAR conflicts: accepted entries only.
  typedef struct {
    const entry *e;
    const char *aar;
  } seen_entry;
  seen_entry *seen = NULL;
  size_t nseen = 0;
  size_t capseen = 0;
  char *result = NULL;
  for (size_t ai = 0; ai < aar_count; ai++) {
    if (aar_texts[ai] == NULL) {
      continue; // manifest-less AAR: legal, merges nothing
    }
    char *aar_err = NULL;
    manifest *lib = parse_manifest(aar_texts[ai], app->package, &aar_err);
    if (lib == NULL) {
      if (err_out != NULL) {
        ccb_str s;
        ccb_str_init(&s);
        ccb_str_append(&s, "AAR ");
        ccb_str_append(&s, aar_names[ai]);
        ccb_str_append(&s, ": malformed AndroidManifest.xml: ");
        ccb_str_append(&s, aar_err != NULL ? aar_err : "parse error");
        *err_out = ccb_str_steal(&s);
      }
      free(aar_err);
      goto done;
    }
    for (size_t ei = 0; ei < lib->nentries; ei++) {
      entry *e = &lib->entries[ei];
      int is_comp = is_component_kind(e->kind);
      // App wins on name conflict.
      int app_conflict = 0;
      int app_dup = 0;
      for (size_t k = 0; k < app->nentries; k++) {
        const entry *ae = &app->entries[k];
        if (entry_same_identity(e, ae)) {
          app_conflict = 1;
          break;
        }
        if (e->resolved == NULL && strcmp(e->kind, ae->kind) == 0 &&
            strcmp(e->norm, ae->norm) == 0) {
          app_dup = 1;
          break;
        }
      }
      if (app_conflict) {
        ccb_str s;
        ccb_str_init(&s);
        ccb_str_append(&s, "AAR ");
        ccb_str_append(&s, aar_names[ai]);
        ccb_str_append(&s, ": ");
        ccb_str_append(&s, e->kind);
        ccb_str_append(&s, " ");
        ccb_str_append(&s, e->resolved);
        ccb_str_append(&s, " already declared in the app manifest; keeping the app's version");
        char *msg = ccb_str_steal(&s);
        warns_push(warns, msg);
        free(msg);
        continue;
      }
      if (app_dup) {
        continue; // identical nameless feature already in the app
      }
      // AAR-vs-AAR: same identity, same content dedups silently;
      // same identity, different content keeps the first with a warning.
      const char *first_aar = NULL;
      const entry *first_entry = NULL;
      for (size_t k = 0; k < nseen; k++) {
        if (entry_same_identity(e, seen[k].e)) {
          first_aar = seen[k].aar;
          first_entry = seen[k].e;
          break;
        }
        // Nameless entries (uses-feature without android:name) have no
        // identity to conflict on; they only deduplicate on the full key.
        if (e->resolved == NULL && seen[k].e->resolved == NULL &&
            strcmp(e->kind, seen[k].e->kind) == 0 &&
            strcmp(e->norm, seen[k].e->norm) == 0) {
          first_aar = seen[k].aar;
          first_entry = seen[k].e;
          break;
        }
      }
      if (first_entry != NULL) {
        if (strcmp(e->norm, first_entry->norm) == 0) {
          continue; // identical duplicate: silent dedup
        }
        ccb_str s;
        ccb_str_init(&s);
        ccb_str_append(&s, "AAR ");
        ccb_str_append(&s, aar_names[ai]);
        ccb_str_append(&s, ": ");
        ccb_str_append(&s, e->kind);
        ccb_str_append(&s, " ");
        ccb_str_append(&s, e->resolved);
        ccb_str_append(&s, " conflicts with the same entry from AAR ");
        ccb_str_append(&s, first_aar);
        ccb_str_append(&s, "; keeping the first");
        char *msg = ccb_str_steal(&s);
        warns_push(warns, msg);
        free(msg);
        continue;
      }
      // Accept: move the entry out of the library manifest so it
      // survives manifest_free below.
      entry *kept = ccb_xmalloc(sizeof(*kept));
      *kept = *e;
      memset(e, 0, sizeof(*e));
      if (is_comp) {
        if (ncomps == capcomps) {
          size_t ncap = capcomps == 0 ? 8 : capcomps * 2;
          comps = ccb_xrealloc(comps, ncap * sizeof(*comps));
          capcomps = ncap;
        }
        comps[ncomps++] = kept;
      } else {
        if (nuses == capuses) {
          size_t ncap = capuses == 0 ? 8 : capuses * 2;
          uses = ccb_xrealloc(uses, ncap * sizeof(*uses));
          capuses = ncap;
        }
        uses[nuses++] = kept;
      }
      if (nseen == capseen) {
        size_t ncap = capseen == 0 ? 8 : capseen * 2;
        seen = ccb_xrealloc(seen, ncap * sizeof(*seen));
        capseen = ncap;
      }
      seen[nseen].e = kept;
      seen[nseen].aar = aar_names[ai];
      nseen++;
    }
    manifest_free(lib);
  }
  if (ncomps > 0 && app->comp_insert == SIZE_MAX) {
    if (err_out != NULL) {
      *err_out = ccb_xstrdup(
          "cannot merge AAR components: app manifest has no <application> element");
    }
    goto done;
  }
  if (nuses > 0 && app->uses_insert == SIZE_MAX) {
    if (err_out != NULL) {
      *err_out = ccb_xstrdup(
          "cannot merge AAR entries: app manifest has no insertion point");
    }
    goto done;
  }
  {
    size_t app_len = strlen(app_text);
    ccb_str out;
    ccb_str_init(&out);
    size_t pos = 0;
    if (nuses > 0) {
      ccb_str_append_n(&out, app_text + pos, app->uses_insert - pos);
      for (size_t i = 0; i < nuses; i++) {
        emit_entry(&out, uses[i]);
      }
      pos = app->uses_insert;
    }
    if (ncomps > 0) {
      ccb_str_append_n(&out, app_text + pos, app->comp_insert - pos);
      for (size_t i = 0; i < ncomps; i++) {
        emit_entry(&out, comps[i]);
      }
      pos = app->comp_insert;
    }
    ccb_str_append(&out, app_text + pos);
    (void)app_len;
    result = ccb_str_steal(&out);
  }
done:
  for (size_t i = 0; i < nuses; i++) {
    entry_free_contents(uses[i]);
    free(uses[i]);
  }
  free(uses);
  for (size_t i = 0; i < ncomps; i++) {
    entry_free_contents(comps[i]);
    free(comps[i]);
  }
  free(comps);
  free(seen);
  manifest_free(app);
  return result;
}
