// ccb_maven_pom: Maven POM subset parser + property substitution.
// Split from ccb_maven.c: the POM text parser and ${...} substitution
// live here; resolution only sees mv_pom through ccb_maven_internal.h.

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_str.h"
#include <ctype.h>
#include "ccb_maven_internal.h"

// ---------- POM subset parser ----------
//
// Only the elements resolution needs: project coordinates, packaging,
// parent coordinates, properties, dependencies, and
// dependencyManagement. Everything else is skipped. The parser is a
// single pass with an element-name stack; it never builds a DOM, so a
// hostile POM cannot make it allocate proportional to anything but
// its own size. Malformed XML is a resolution error, never a crash:
// unknown content is skipped, mismatched tags fail the parse.

// One <profile>: id, <activeByDefault> activation, and the
// properties, dependencies, and dependencyManagement it contributes
// when active. Other activation forms (jdk, os, property, file)
// are parsed nowhere: a profile using them is inactive unless its
// id is listed in [deps] maven_profiles.

static int mv_path_is(mv_parser *x, const char *n0, const char *n1,
    const char *n2, const char *n3, const char *n4) {
  const char *want[5];
  want[0] = n0;
  want[1] = n1;
  want[2] = n2;
  want[3] = n3;
  want[4] = n4;
  int depth = 0;
  while (depth < 5 && want[depth] != NULL) {
    depth++;
  }
  if (x->depth != depth) {
    return 0;
  }
  for (int i = 0; i < depth; i++) {
    if (strcmp(x->stack[i], want[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static void mv_set_field(char **field, const char *val) {
  // First occurrence wins within one element; duplicate fields in a
  // single dependency are malformed POMs, but tolerating them by
  // keeping the first is safer than failing a real-world file.
  if (*field == NULL) {
    *field = (char *)val;
  }
}

// Decode a numeric character reference (&#...; or &#x...;). Returns 1
// with *out set when the value is a valid ASCII character, else 0.
// An invalid digit or an out-of-range value fails the whole entity,
// so the caller keeps the '&' literally.
static int mv_numeric_entity(const char *ent, size_t elen, char *out) {
  long v = 0;
  int ok = 1;
  if (ent[1] == 'x' || ent[1] == 'X') {
    for (size_t k = 2; k < elen; k++) {
      char ch = ent[k];
      v *= 16;
      if (ch >= '0' && ch <= '9') {
        v += ch - '0';
      } else if (ch >= 'a' && ch <= 'f') {
        v += ch - 'a' + 10;
      } else if (ch >= 'A' && ch <= 'F') {
        v += ch - 'A' + 10;
      } else {
        ok = 0;
        break;
      }
    }
  } else {
    for (size_t k = 1; k < elen; k++) {
      if (ent[k] < '0' || ent[k] > '9') {
        ok = 0;
        break;
      }
      v = v * 10 + (ent[k] - '0');
    }
  }
  if (ok && v > 0 && v < 0x80) {
    *out = (char)v;
    return 1;
  }
  return 0;
}

// Decode one entity body (between '&' and ';'): the five named
// entities, or a numeric reference. Returns 1 with *out set, else 0.
static int mv_entity_char(const char *ent, size_t elen, char *out) {
  if (elen == 3 && memcmp(ent, "amp", 3) == 0) {
    *out = '&';
    return 1;
  }
  if (elen == 2 && memcmp(ent, "lt", 2) == 0) {
    *out = '<';
    return 1;
  }
  if (elen == 2 && memcmp(ent, "gt", 2) == 0) {
    *out = '>';
    return 1;
  }
  if (elen == 4 && memcmp(ent, "quot", 4) == 0) {
    *out = '"';
    return 1;
  }
  if (elen == 4 && memcmp(ent, "apos", 4) == 0) {
    *out = '\'';
    return 1;
  }
  if (elen > 1 && ent[0] == '#') {
    return mv_numeric_entity(ent, elen, out);
  }
  return 0;
}

// Try to decode the entity starting at text[i] ('&'). Returns 1 with
// *out set and *next past the ';' on success, else 0.
static int mv_try_entity(const char *text, size_t len, size_t i, char *out,
    size_t *next) {
  size_t semi = i + 1;
  while (semi < len && text[semi] != ';' && semi - i <= 10) {
    semi++;
  }
  if (semi >= len || text[semi] != ';') {
    return 0;
  }
  if (!mv_entity_char(text + i + 1, semi - (i + 1), out)) {
    return 0;
  }
  *next = semi + 1;
  return 1;
}

static char *mv_decode_entities(mv_parser *x, const char *text, size_t len) {
  ccb_str s;
  ccb_str_init(&s);
  size_t i = 0;
  while (i < len) {
    if (text[i] == '&') {
      char ch = '\0';
      size_t next = 0;
      if (mv_try_entity(text, len, i, &ch, &next)) {
        ccb_str_append_c(&s, ch);
        i = next;
        continue;
      }
      // Unknown or malformed entity: keep the '&' literally.
      ccb_str_append_c(&s, '&');
      i++;
      continue;
    }
    ccb_str_append_c(&s, text[i]);
    i++;
  }
  char *out = ccb_arena_strdup(x->a, s.buf);
  ccb_str_free(&s);
  return out;
}

// Trim ASCII whitespace from both ends of an arena string in place.
char *mv_trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
    s++;
  }
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' ||
      s[n - 1] == '\r')) {
    s[--n] = '\0';
  }
  return s;
}

static void mv_text(mv_parser *x, const char *text, size_t len) {
  char *decoded = mv_decode_entities(x, text, len);
  char *val = mv_trim(decoded);
  if (val[0] == '\0') {
    return;
  }
  mv_pom *pom = x->pom;
  if (mv_path_is(x, "project", "packaging", NULL, NULL, NULL)) {
    mv_set_field(&pom->packaging, val);
  } else if (mv_path_is(x, "project", "groupId", NULL, NULL, NULL)) {
    mv_set_field(&pom->group, val);
  } else if (mv_path_is(x, "project", "artifactId", NULL, NULL, NULL)) {
    mv_set_field(&pom->artifact, val);
  } else if (mv_path_is(x, "project", "version", NULL, NULL, NULL)) {
    mv_set_field(&pom->version, val);
  } else if (mv_path_is(x, "project", "parent", "groupId", NULL, NULL)) {
    mv_set_field(&pom->parent_group, val);
  } else if (mv_path_is(x, "project", "parent", "artifactId", NULL, NULL)) {
    mv_set_field(&pom->parent_artifact, val);
  } else if (mv_path_is(x, "project", "parent", "version", NULL, NULL)) {
    mv_set_field(&pom->parent_version, val);
  } else if (x->depth == 3 && strcmp(x->stack[0], "project") == 0 &&
      strcmp(x->stack[1], "properties") == 0) {
    // A direct child of <properties>: the element name is the key.
    // Later duplicates keep the first value, like Maven's ordering.
    for (size_t i = 0; i < pom->props_count; i++) {
      if (strcmp(pom->props[i].key, x->stack[2]) == 0) {
        return;
      }
    }
    MV_GROW(x->a, pom->props, pom->props_count, pom->props_cap, mv_prop);
    pom->props[pom->props_count].key = ccb_arena_strdup(x->a, x->stack[2]);
    pom->props[pom->props_count].val = val;
    pom->props_count++;
  } else if (mv_path_is(x, "project", "profiles", "profile", "id", NULL)) {
    if (x->cur_profile != NULL) {
      mv_set_field(&x->cur_profile->id, val);
    }
  } else if (mv_path_is(x, "project", "profiles", "profile", "activation",
      "activeByDefault")) {
    if (x->cur_profile != NULL) {
      x->cur_profile->active_default = strcmp(val, "true") == 0;
    }
  } else if (x->depth == 5 && strcmp(x->stack[0], "project") == 0 &&
      strcmp(x->stack[1], "profiles") == 0 &&
      strcmp(x->stack[2], "profile") == 0 &&
      strcmp(x->stack[3], "properties") == 0) {
    // A direct child of a profile's <properties>: same first-wins
    // rule as the POM's own properties.
    if (x->cur_profile != NULL) {
      mv_profile *prof = x->cur_profile;
      int seen = 0;
      for (size_t i = 0; i < prof->props_count; i++) {
        if (strcmp(prof->props[i].key, x->stack[4]) == 0) {
          seen = 1;
          break;
        }
      }
      if (!seen) {
        MV_GROW(x->a, prof->props, prof->props_count, prof->props_cap,
          mv_prop);
        prof->props[prof->props_count].key =
          ccb_arena_strdup(x->a, x->stack[4]);
        prof->props[prof->props_count].val = val;
        prof->props_count++;
      }
    }
  } else if (x->cur != NULL) {
    // Text arrives with the field element still on the stack, so a
    // dependency field sits exactly one level below its container:
    // depth 4 (plain), 5 (managed), 6 (profile), or 7 (profile
    // managed). Anything deeper (e.g. text inside <exclusions>) is
    // not a dependency field and is ignored.
    int field_depth = x->cur_where == 0 ? 4 : x->cur_where == 1 ? 5 :
      x->cur_where == 2 ? 6 : 7;
    if (x->depth == field_depth) {
      const char *field = x->stack[x->depth - 1];
      if (strcmp(field, "groupId") == 0) {
        mv_set_field(&x->cur->group, val);
      } else if (strcmp(field, "artifactId") == 0) {
        mv_set_field(&x->cur->artifact, val);
      } else if (strcmp(field, "version") == 0) {
        mv_set_field(&x->cur->version, val);
      } else if (strcmp(field, "scope") == 0) {
        mv_set_field(&x->cur->scope, val);
      } else if (strcmp(field, "optional") == 0) {
        mv_set_field(&x->cur->optional, val);
      }
    }
  }
}

static int mv_is_name_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' ||
    ch == ':';
}

// Read an element name starting at *pp (past '<' or '</'), strip any
// namespace prefix, and leave *pp after the name. Returns 0 on a
// missing or over-long name.
static int mv_read_name(mv_parser *x, const char **pp, char *out, size_t out_size) {
  const char *p = *pp;
  const char *start = p;
  while (p < x->end && mv_is_name_char(*p)) {
    p++;
  }
  if (p == start) {
    return 0;
  }
  // Strip namespace prefix: the local name is what paths match on.
  const char *local = start;
  for (const char *q = start; q < p; q++) {
    if (*q == ':') {
      local = q + 1;
    }
  }
  size_t n = (size_t)(p - local);
  if (n == 0 || n >= out_size) {
    return 0;
  }
  memcpy(out, local, n);
  out[n] = '\0';
  *pp = p;
  return 1;
}

static void mv_open(mv_parser *x, const char *name) {
  if (x->depth >= 32) {
    x->failed = 1;
    return;
  }
  memcpy(x->stack[x->depth], name, strlen(name) + 1);
  x->depth++;
  if (mv_path_is(x, "project", "profiles", "profile", NULL, NULL)) {
    x->cur_profile = ccb_arena_alloc(x->a, sizeof(*x->cur_profile));
    memset(x->cur_profile, 0, sizeof(*x->cur_profile));
  } else if (mv_path_is(x, "project", "dependencies", "dependency", NULL, NULL)) {
    x->cur = ccb_arena_alloc(x->a, sizeof(*x->cur));
    memset(x->cur, 0, sizeof(*x->cur));
    x->cur_where = 0;
  } else if (mv_path_is(x, "project", "dependencyManagement", "dependencies",
      "dependency", NULL)) {
    x->cur = ccb_arena_alloc(x->a, sizeof(*x->cur));
    memset(x->cur, 0, sizeof(*x->cur));
    x->cur_where = 1;
  } else if (mv_path_is(x, "project", "profiles", "profile", "dependencies",
      "dependency")) {
    x->cur = ccb_arena_alloc(x->a, sizeof(*x->cur));
    memset(x->cur, 0, sizeof(*x->cur));
    x->cur_where = 2;
  }
  // mv_path_is only matches five levels, so the profile-managed
  // dependency (project/profiles/profile/dependencyManagement/
  // dependencies/dependency, depth 6) is matched by hand.
  if (x->depth == 6 && strcmp(x->stack[0], "project") == 0 &&
      strcmp(x->stack[1], "profiles") == 0 &&
      strcmp(x->stack[2], "profile") == 0 &&
      strcmp(x->stack[3], "dependencyManagement") == 0 &&
      strcmp(x->stack[4], "dependencies") == 0 &&
      strcmp(x->stack[5], "dependency") == 0) {
    x->cur = ccb_arena_alloc(x->a, sizeof(*x->cur));
    memset(x->cur, 0, sizeof(*x->cur));
    x->cur_where = 3;
  }
}

// True when the element now closing is a <dependency> in one of the
// four containers (plain, managed, profile, profile-managed).
// mv_path_is only matches five levels, so the six-level
// profile-managed path is matched by hand.
static int mv_is_dep_close(mv_parser *x) {
  if (x->depth == 3 && strcmp(x->stack[0], "project") == 0 &&
      strcmp(x->stack[1], "dependencies") == 0 &&
      strcmp(x->stack[2], "dependency") == 0) {
    return 1;
  }
  if (x->depth == 4 && strcmp(x->stack[0], "project") == 0 &&
      strcmp(x->stack[1], "dependencyManagement") == 0 &&
      strcmp(x->stack[2], "dependencies") == 0 &&
      strcmp(x->stack[3], "dependency") == 0) {
    return 1;
  }
  if (x->depth == 5 && strcmp(x->stack[0], "project") == 0 &&
      strcmp(x->stack[1], "profiles") == 0 &&
      strcmp(x->stack[2], "profile") == 0 &&
      strcmp(x->stack[3], "dependencies") == 0 &&
      strcmp(x->stack[4], "dependency") == 0) {
    return 1;
  }
  if (x->depth == 6 && strcmp(x->stack[0], "project") == 0 &&
      strcmp(x->stack[1], "profiles") == 0 &&
      strcmp(x->stack[2], "profile") == 0 &&
      strcmp(x->stack[3], "dependencyManagement") == 0 &&
      strcmp(x->stack[4], "dependencies") == 0 &&
      strcmp(x->stack[5], "dependency") == 0) {
    return 1;
  }
  return 0;
}

static void mv_close(mv_parser *x, const char *name) {
  if (x->depth == 0 || strcmp(x->stack[x->depth - 1], name) != 0) {
    x->failed = 1;
    return;
  }
  if (x->cur != NULL && mv_is_dep_close(x)) {
    mv_pom *pom = x->pom;
    if (x->cur_where == 0) {
      MV_GROW(x->a, pom->deps, pom->deps_count, pom->deps_cap, mv_dep);
      pom->deps[pom->deps_count++] = *x->cur;
    } else if (x->cur_where == 1) {
      MV_GROW(x->a, pom->mgmt, pom->mgmt_count, pom->mgmt_cap, mv_dep);
      pom->mgmt[pom->mgmt_count++] = *x->cur;
    } else if (x->cur_where == 2 && x->cur_profile != NULL) {
      mv_profile *prof = x->cur_profile;
      MV_GROW(x->a, prof->deps, prof->deps_count, prof->deps_cap, mv_dep);
      prof->deps[prof->deps_count++] = *x->cur;
    } else if (x->cur_where == 3 && x->cur_profile != NULL) {
      mv_profile *prof = x->cur_profile;
      MV_GROW(x->a, prof->mgmt, prof->mgmt_count, prof->mgmt_cap, mv_dep);
      prof->mgmt[prof->mgmt_count++] = *x->cur;
    }
    x->cur = NULL;
  }
  if (x->cur_profile != NULL &&
      mv_path_is(x, "project", "profiles", "profile", NULL, NULL)) {
    mv_pom *pom = x->pom;
    MV_GROW(x->a, pom->profiles, pom->profiles_count, pom->profiles_cap,
      mv_profile);
    pom->profiles[pom->profiles_count++] = *x->cur_profile;
    x->cur_profile = NULL;
  }
  x->depth--;
}

// Find the end of a "<!-- ... -->" comment starting at q (past
// "<!--"). Returns the pointer past "-->", or NULL when unterminated.
static const char *mv_skip_comment(const char *end, const char *q) {
  for (; q + 2 < end; q++) {
    if (memcmp(q, "-->", 3) == 0) {
      return q + 3;
    }
  }
  return NULL;
}

// Find the end of a "<? ... ?>" processing instruction starting at q
// (past "<?"). Returns the pointer past "?>", or NULL when unterminated.
static const char *mv_skip_pi(const char *end, const char *q) {
  for (; q + 1 < end; q++) {
    if (memcmp(q, "?>", 2) == 0) {
      return q + 2;
    }
  }
  return NULL;
}

// Find the end of a "<! ... >" declaration starting at q (past "<!").
// Skips to '>', honoring an internal subset in [ ... ]. Returns the
// pointer past '>', or NULL when unterminated.
static const char *mv_skip_decl(const char *end, const char *q) {
  int bracket = 0;
  while (q < end) {
    if (*q == '[') {
      bracket = 1;
    } else if (*q == ']') {
      bracket = 0;
    } else if (*q == '>' && !bracket) {
      return q + 1;
    }
    q++;
  }
  return NULL;
}

// Parse a "</name ...>" close tag starting at p (past '<', at '/').
// Advances x->p past '>' and closes the element. Returns 0 on
// success, -1 on malformed XML.
static int mv_parse_close_tag(mv_parser *x, const char *p) {
  const char *q = p + 1;
  char name[64];
  if (!mv_read_name(x, &q, name, sizeof(name))) {
    return -1;
  }
  while (q < x->end && *q != '>') {
    q++;
  }
  if (q >= x->end) {
    return -1;
  }
  mv_close(x, name);
  x->p = q + 1;
  return 0;
}

// Parse an open tag starting at p (past '<'). Reads the name, skips
// attributes quote-aware, and detects a self-closing "/>". Advances
// x->p and opens (and, for self-closing tags, closes) the element.
// Returns 0 on success, -1 on malformed XML.
static int mv_parse_open_tag(mv_parser *x, const char *p) {
  const char *q = p;
  char name[64];
  if (!mv_read_name(x, &q, name, sizeof(name))) {
    return -1;
  }
  int self_close = 0;
  while (q < x->end) {
    char ch = *q;
    if (ch == '"' || ch == '\'') {
      const char *e = memchr(q + 1, ch, (size_t)(x->end - (q + 1)));
      if (e == NULL) {
        return -1;
      }
      q = e + 1;
      continue;
    }
    if (ch == '/' && q + 1 < x->end && q[1] == '>') {
      self_close = 1;
      q += 2;
      break;
    }
    if (ch == '>') {
      q++;
      break;
    }
    q++;
  }
  if (q > x->end) {
    return -1;
  }
  mv_open(x, name);
  if (self_close) {
    mv_close(x, name);
  }
  x->p = q;
  return 0;
}

// Parse one markup item starting at x->p ('<'): a comment, a
// processing instruction, a declaration, a close tag, or an open tag.
// Advances x->p past the item. Returns 0 on success, -1 on
// malformed XML.
static int mv_parse_markup(mv_parser *x) {
  const char *p = x->p + 1;
  if (p + 2 < x->end && memcmp(p, "!--", 3) == 0) {
    const char *e = mv_skip_comment(x->end, p + 3);
    if (e == NULL) {
      return -1;
    }
    x->p = e;
    return 0;
  }
  if (p < x->end && *p == '?') {
    const char *e = mv_skip_pi(x->end, p + 1);
    if (e == NULL) {
      return -1;
    }
    x->p = e;
    return 0;
  }
  if (p < x->end && *p == '!') {
    // DOCTYPE or another declaration: skip to '>', honoring an
    // internal subset in [ ... ].
    const char *e = mv_skip_decl(x->end, p + 1);
    if (e == NULL) {
      return -1;
    }
    x->p = e;
    return 0;
  }
  if (p < x->end && *p == '/') {
    return mv_parse_close_tag(x, p);
  }
  // Open tag: read the name, skip attributes quote-aware, and
  // detect a self-closing "/>".
  return mv_parse_open_tag(x, p);
}

// Parse a POM document. Returns 0 on success, -1 on malformed XML.
int mv_parse_pom(ccb_arena *a, const char *text, size_t len, mv_pom *pom) {
  mv_parser x;
  memset(&x, 0, sizeof(x));
  x.a = a;
  x.p = text;
  x.end = text + len;
  x.pom = pom;
  memset(pom, 0, sizeof(*pom));
  while (x.p < x.end && !x.failed) {
    if (*x.p == '<') {
      if (mv_parse_markup(&x) != 0) {
        return -1;
      }
      continue;
    }
    const char *lt = memchr(x.p, '<', (size_t)(x.end - x.p));
    const char *text_end = lt != NULL ? lt : x.end;
    if (text_end > x.p) {
      mv_text(&x, x.p, (size_t)(text_end - x.p));
    }
    x.p = text_end;
  }
  if (x.failed || x.depth != 0 || x.cur != NULL) {
    return -1;
  }
  return 0;
}

// ---------- property substitution ----------

static const char *mv_prop_get(mv_pom *pom, const char *key) {
  for (size_t i = 0; i < pom->props_count; i++) {
    if (strcmp(pom->props[i].key, key) == 0) {
      return pom->props[i].val;
    }
  }
  return NULL;
}

// Expand ${...} references: same-POM properties first, then
// project.groupId / project.artifactId / project.version (the
// effective coordinates, so a child POM's ${project.version} tracks
// its parent-inherited version). Unknown names fail (*bad = 1).
// Nested references resolve over up to 8 passes; anything left after
// that is a cycle and fails too.
char *mv_subst(ccb_arena *a, mv_pom *pom, const char *eff_group,
    const char *eff_artifact, const char *eff_version, const char *value,
    int *bad) {
  *bad = 0;
  if (value == NULL) {
    return NULL;
  }
  char *cur = ccb_arena_strdup(a, value);
  for (int pass = 0; pass < 8; pass++) {
    const char *dollar = strstr(cur, "${");
    if (dollar == NULL) {
      return cur;
    }
    ccb_str s;
    ccb_str_init(&s);
    const char *p = cur;
    int replaced = 0;
    while (*p != '\0') {
      const char *d = strstr(p, "${");
      if (d == NULL) {
        ccb_str_append(&s, p);
        break;
      }
      ccb_str_append_n(&s, p, (size_t)(d - p));
      const char *close = strchr(d + 2, '}');
      if (close == NULL) {
        ccb_str_append(&s, d);
        break;
      }
      size_t nlen = (size_t)(close - (d + 2));
      char *name = ccb_arena_strndup(a, d + 2, nlen);
      const char *rep = mv_prop_get(pom, name);
      if (rep == NULL && strcmp(name, "project.groupId") == 0) {
        rep = eff_group;
      }
      if (rep == NULL && strcmp(name, "project.artifactId") == 0) {
        rep = eff_artifact;
      }
      if (rep == NULL && strcmp(name, "project.version") == 0) {
        rep = eff_version;
      }
      if (rep == NULL) {
        ccb_str_free(&s);
        *bad = 1;
        return NULL;
      }
      ccb_str_append(&s, rep);
      replaced = 1;
      p = close + 1;
    }
    cur = ccb_arena_strdup(a, s.buf);
    ccb_str_free(&s);
    if (!replaced) {
      // No reference expanded but one is still present: it is
      // unterminated ("${foo" with no '}'), not a literal.
      if (strstr(cur, "${") != NULL) {
        *bad = 1;
        return NULL;
      }
      return cur;
    }
  }
  if (strstr(cur, "${") != NULL) {
    *bad = 1;
    return NULL;
  }
  return cur;
}
