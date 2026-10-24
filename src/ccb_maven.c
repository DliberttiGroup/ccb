// ccb_maven: remote Maven dependency resolution (see ccb_maven.h).
#include "ccb_common.h"

#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_str.h"

#include <ctype.h>

// Growable arena arrays: appends copy into a fresh arena block when
// full. Counts here are small (dependencies per project), so the
// copy cost is negligible and no array ever needs individual freeing.
#define MV_GROW(arena, arr, count, cap, type) do { \
    if ((count) == (cap)) { \
      size_t mv_ncap = (cap) == 0 ? 8 : (cap) * 2; \
      type *mv_narr = ccb_arena_alloc((arena), mv_ncap * sizeof(type)); \
      if ((count) > 0) { \
        memcpy(mv_narr, (arr), (count) * sizeof(type)); \
      } \
      (arr) = mv_narr; \
      (cap) = mv_ncap; \
    } \
  } while (0)

static void mv_diag(ccb_diag_list *diags, const char *code, const char *coord,
    const char *msg) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "maven");
  if (coord != NULL && coord[0] != '\0') {
    ccb_str_append(&s, " ");
    ccb_str_append(&s, coord);
  }
  ccb_str_append(&s, ": ");
  ccb_str_append(&s, msg);
  ccb_diag d = ccb_diag_make(code, "maven", s.buf);
  ccb_diag_list_push(diags, &d);
  ccb_str_free(&s);
}

// ---------- coordinates ----------

static int mv_part_ok(const char *s, size_t n) {
  if (n == 0) {
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch <= ' ' || ch == '/' || ch == '\\' || ch == ':' || ch == 0x7f) {
      return 0;
    }
  }
  // Reject "." and ".." outright so no coordinate can escape the
  // cache directory through a crafted group or version.
  if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) {
    return 0;
  }
  return 1;
}

int ccb_maven_parse_coord(const char *s, char **group, char **artifact, char **version) {
  if (s == NULL) {
    return 0;
  }
  const char *c1 = strchr(s, ':');
  const char *c2 = c1 != NULL ? strchr(c1 + 1, ':') : NULL;
  if (c1 == NULL || c2 == NULL || strchr(c2 + 1, ':') != NULL) {
    return 0;
  }
  if (!mv_part_ok(s, (size_t)(c1 - s)) ||
      !mv_part_ok(c1 + 1, (size_t)(c2 - (c1 + 1))) ||
      !mv_part_ok(c2 + 1, strlen(c2 + 1))) {
    return 0;
  }
  *group = ccb_xstrndup(s, (size_t)(c1 - s));
  *artifact = ccb_xstrndup(c1 + 1, (size_t)(c2 - (c1 + 1)));
  *version = ccb_xstrdup(c2 + 1);
  return 1;
}

static char *mv_group_path(ccb_arena *a, const char *group) {
  char *p = ccb_arena_strdup(a, group);
  for (char *q = p; *q != '\0'; q++) {
    if (*q == '.') {
      *q = '/';
    }
  }
  return p;
}

static char *mv_coord_str(ccb_arena *a, const char *group, const char *artifact,
    const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, group);
  ccb_str_append_c(&s, ':');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, ':');
  ccb_str_append(&s, version);
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

// ---------- repositories and cache ----------

#define MV_GOOGLE_MAVEN "https://dl.google.com/dl/android/maven2/"
#define MV_MAVEN_CENTRAL "https://repo.maven.apache.org/maven2/"

static char *mv_norm_base(ccb_arena *a, const char *base) {
  // Trim surrounding whitespace, then ensure exactly one trailing '/'.
  while (*base == ' ' || *base == '\t' || *base == '\n' || *base == '\r') {
    base++;
  }
  size_t n = strlen(base);
  while (n > 0 && (base[n - 1] == ' ' || base[n - 1] == '\t' ||
      base[n - 1] == '\n' || base[n - 1] == '\r')) {
    n--;
  }
  while (n > 0 && base[n - 1] == '/') {
    n--;
  }
  char *out = ccb_arena_strndup(a, base, n);
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, out);
  ccb_str_append_c(&s, '/');
  // The joined string is short-lived arena memory; steal it back into
  // the arena by duplicating, since ccb_str owns its buffer.
  char *norm = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return norm;
}

// Repository list: CCB_MAVEN_REPOS (comma-separated, replaces the
// defaults; the test seam for fixture file:// repos) or Google Maven
// plus Maven Central when the project opts in.
static char **mv_repos(ccb_arena *a, const ccb_config *cfg, size_t *count) {
  char **repos = NULL;
  size_t n = 0;
  size_t cap = 0;
  const char *env = getenv("CCB_MAVEN_REPOS");
  if (env != NULL && env[0] != '\0') {
    const char *p = env;
    while (*p != '\0') {
      const char *comma = strchr(p, ',');
      size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
      // Skip entries that are only whitespace.
      size_t t = 0;
      while (t < len && (p[t] == ' ' || p[t] == '\t')) {
        t++;
      }
      if (t < len) {
        char *raw = ccb_arena_strndup(a, p + t, len - t);
        MV_GROW(a, repos, n, cap, char *);
        repos[n++] = mv_norm_base(a, raw);
      }
      p = comma != NULL ? comma + 1 : p + len;
    }
  }
  if (n == 0) {
    MV_GROW(a, repos, n, cap, char *);
    repos[n++] = mv_norm_base(a, MV_GOOGLE_MAVEN);
    if (cfg->maven_central) {
      MV_GROW(a, repos, n, cap, char *);
      repos[n++] = mv_norm_base(a, MV_MAVEN_CENTRAL);
    }
  }
  *count = n;
  return repos;
}

char *ccb_maven_cache_dir(void) {
  const char *env = getenv("CCB_MAVEN_CACHE");
  if (env != NULL && env[0] != '\0') {
    return ccb_xstrdup(env);
  }
  char *root = ccb_net_cache_root();
  if (root == NULL) {
    return NULL;
  }
  char *dir = ccb_path_join(root, "maven2");
  free(root);
  return dir;
}

// Cache layout mirrors the repository layout:
// <cache>/<group-path>/<artifact>/<version>/<artifact>-<version>.<ext>
static char *mv_cache_file(ccb_arena *a, const char *cache, const char *group,
    const char *artifact, const char *version, const char *leaf) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, cache);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, mv_group_path(a, group));
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, version);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, leaf);
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

static char *mv_leaf(ccb_arena *a, const char *artifact, const char *version,
    const char *ext) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '-');
  ccb_str_append(&s, version);
  ccb_str_append_c(&s, '.');
  ccb_str_append(&s, ext);
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

static char *mv_url(ccb_arena *a, const char *base, const char *group,
    const char *artifact, const char *version, const char *leaf) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, base);
  ccb_str_append(&s, mv_group_path(a, group));
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, version);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, leaf);
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

// ---------- checksums ----------

// Read a published checksum file: first whitespace-separated token,
// lowercased. Returns 1 when the token is plausible hex of length 40
// (SHA-1) or 64 (SHA-256), else 0.
static int mv_read_checksum(const char *path, char *out, size_t out_size) {
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &text, &len) != CCB_OK) {
    return 0;
  }
  size_t i = 0;
  while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
      text[i] == '\r')) {
    i++;
  }
  size_t j = i;
  while (j < len && text[j] != ' ' && text[j] != '\t' && text[j] != '\n' &&
      text[j] != '\r') {
    j++;
  }
  size_t n = j - i;
  int ok = (n == 40 || n == 64) && n + 1 <= out_size;
  if (ok) {
    for (size_t k = 0; k < n; k++) {
      char ch = text[i + k];
      if (!isxdigit((unsigned char)ch)) {
        ok = 0;
        break;
      }
      out[k] = (char)tolower((unsigned char)ch);
    }
  }
  if (ok) {
    out[n] = '\0';
  }
  free(text);
  return ok;
}

// Verify a downloaded file against a published checksum for its URL,
// trying .sha256 first, then .sha1. Returns 0 when verified or when
// the repo publishes no checksum (the caller then records the SHA-256
// in a sidecar), 1 on mismatch. Downloads go to a temp sibling path
// that is always removed here.
static int mv_verify_downloaded(ccb_arena *a, const char *tmp_path,
    const char *url, char *sha256_hex) {
  if (ccb_sha256_file(tmp_path, sha256_hex) != CCB_OK) {
    return 1;
  }
  static const char *suffixes[] = { ".sha256", ".sha1" };
  for (size_t s = 0; s < 2; s++) {
    ccb_str us;
    ccb_str_init(&us);
    ccb_str_append(&us, url);
    ccb_str_append(&us, suffixes[s]);
    char *curl_url = ccb_arena_strdup(a, us.buf);
    ccb_str_free(&us);
    ccb_str ts;
    ccb_str_init(&ts);
    ccb_str_append(&ts, tmp_path);
    ccb_str_append(&ts, ".checksum");
    char *chk_path = ccb_arena_strdup(a, ts.buf);
    ccb_str_free(&ts);
    int dl = ccb_net_download(curl_url, chk_path);
    if (dl != 0) {
      // No checksum file at this suffix: try the next one, or accept
      // the download as checksum-less.
      remove(chk_path);
      continue;
    }
    char token[65];
    int have = mv_read_checksum(chk_path, token, sizeof(token));
    remove(chk_path);
    if (!have) {
      // Unparseable checksum file: treat as unpublished rather than
      // failing an otherwise good download.
      continue;
    }
    if (strlen(token) == 64) {
      return strcmp(token, sha256_hex) == 0 ? 0 : 1;
    }
    char sha1_hex[41];
    if (ccb_sha1_file(tmp_path, sha1_hex) != CCB_OK) {
      return 1;
    }
    return strcmp(token, sha1_hex) == 0 ? 0 : 1;
  }
  return 0;
}

// Write the recorded SHA-256 sidecar next to a cached file. Best
// effort: a failed write never fails the resolution itself.
static void mv_write_sidecar(const char *final_path, const char *sha256_hex) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, final_path);
  ccb_str_append(&s, ".ccb-sha256");
  char *side = ccb_str_steal(&s);
  ccb_str hex;
  ccb_str_init(&hex);
  ccb_str_append(&hex, sha256_hex);
  ccb_str_append_c(&hex, '\n');
  // Ignore the result: verification on later hits is opportunistic.
  ccb_write_file(side, hex.buf, strlen(hex.buf));
  ccb_str_free(&hex);
  free(side);
}

// Verify a cached file against its recorded sidecar when one exists.
// Returns 1 when the file is good (or no sidecar exists), 0 when the
// recorded hash mismatches. A missing sidecar is adopted: the hash
// is computed and recorded for later hits, unless readonly (explain
// and dry-run modes never write).
static int mv_cache_file_ok(const char *final_path, int readonly) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, final_path);
  ccb_str_append(&s, ".ccb-sha256");
  char *side = ccb_str_steal(&s);
  int ok = 1;
  if (ccb_path_exists(side)) {
    char token[65];
    if (mv_read_checksum(side, token, sizeof(token)) && strlen(token) == 64) {
      char hex[65];
      ok = ccb_sha256_file(final_path, hex) == CCB_OK &&
        strcmp(hex, token) == 0;
    }
  } else if (!readonly) {
    char hex[65];
    if (ccb_sha256_file(final_path, hex) == CCB_OK) {
      mv_write_sidecar(final_path, hex);
    }
  }
  free(side);
  return ok;
}

// ---------- POM subset parser ----------
//
// Only the elements resolution needs: project coordinates, packaging,
// parent coordinates, properties, dependencies, and
// dependencyManagement. Everything else is skipped. The parser is a
// single pass with an element-name stack; it never builds a DOM, so a
// hostile POM cannot make it allocate proportional to anything but
// its own size. Malformed XML is a resolution error, never a crash:
// unknown content is skipped, mismatched tags fail the parse.

typedef struct {
  char *group;
  char *artifact;
  char *version;
  char *scope;
  char *optional;
} mv_dep;

typedef struct {
  char *key;
  char *val;
} mv_prop;

typedef struct {
  char *packaging;
  char *group;
  char *artifact;
  char *version;
  char *parent_group;
  char *parent_artifact;
  char *parent_version;
  mv_prop *props;
  size_t props_count;
  size_t props_cap;
  mv_dep *deps;
  size_t deps_count;
  size_t deps_cap;
  mv_dep *mgmt;
  size_t mgmt_count;
  size_t mgmt_cap;
} mv_pom;

typedef struct {
  ccb_arena *a;
  const char *p;
  const char *end;
  char stack[32][64];
  int depth;
  mv_pom *pom;
  mv_dep *cur;
  int cur_mgmt;
  int failed;
} mv_parser;

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

static char *mv_decode_entities(mv_parser *x, const char *text, size_t len) {
  ccb_str s;
  ccb_str_init(&s);
  size_t i = 0;
  while (i < len) {
    if (text[i] == '&') {
      size_t semi = i + 1;
      while (semi < len && text[semi] != ';' && semi - i <= 10) {
        semi++;
      }
      if (semi < len && text[semi] == ';') {
        size_t elen = semi - (i + 1);
        const char *ent = text + i + 1;
        if (elen == 3 && memcmp(ent, "amp", 3) == 0) {
          ccb_str_append_c(&s, '&');
          i = semi + 1;
          continue;
        }
        if (elen == 2 && memcmp(ent, "lt", 2) == 0) {
          ccb_str_append_c(&s, '<');
          i = semi + 1;
          continue;
        }
        if (elen == 2 && memcmp(ent, "gt", 2) == 0) {
          ccb_str_append_c(&s, '>');
          i = semi + 1;
          continue;
        }
        if (elen == 4 && memcmp(ent, "quot", 4) == 0) {
          ccb_str_append_c(&s, '"');
          i = semi + 1;
          continue;
        }
        if (elen == 4 && memcmp(ent, "apos", 4) == 0) {
          ccb_str_append_c(&s, '\'');
          i = semi + 1;
          continue;
        }
        if (elen > 1 && ent[0] == '#') {
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
            ccb_str_append_c(&s, (char)v);
            i = semi + 1;
            continue;
          }
        }
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
static char *mv_trim(char *s) {
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
  } else if (x->cur != NULL) {
    // Text arrives with the field element still on the stack, so a
    // dependency field sits at depth 4 (plain) or 5 (managed).
    const char *field = NULL;
    if (x->depth == 4 && strcmp(x->stack[0], "project") == 0 &&
        strcmp(x->stack[1], "dependencies") == 0 &&
        strcmp(x->stack[2], "dependency") == 0) {
      field = x->stack[3];
    } else if (x->depth == 5 && strcmp(x->stack[0], "project") == 0 &&
        strcmp(x->stack[1], "dependencyManagement") == 0 &&
        strcmp(x->stack[2], "dependencies") == 0 &&
        strcmp(x->stack[3], "dependency") == 0) {
      field = x->stack[4];
    }
    if (field != NULL) {
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
  if (mv_path_is(x, "project", "dependencies", "dependency", NULL, NULL)) {
    x->cur = ccb_arena_alloc(x->a, sizeof(*x->cur));
    memset(x->cur, 0, sizeof(*x->cur));
    x->cur_mgmt = 0;
  } else if (mv_path_is(x, "project", "dependencyManagement", "dependencies",
      "dependency", NULL)) {
    x->cur = ccb_arena_alloc(x->a, sizeof(*x->cur));
    memset(x->cur, 0, sizeof(*x->cur));
    x->cur_mgmt = 1;
  }
}

static void mv_close(mv_parser *x, const char *name) {
  if (x->depth == 0 || strcmp(x->stack[x->depth - 1], name) != 0) {
    x->failed = 1;
    return;
  }
  if (x->cur != NULL &&
      ((mv_path_is(x, "project", "dependencies", "dependency", NULL, NULL) &&
        !x->cur_mgmt) ||
      (mv_path_is(x, "project", "dependencyManagement", "dependencies",
        "dependency", NULL) && x->cur_mgmt))) {
    mv_pom *pom = x->pom;
    if (x->cur_mgmt) {
      MV_GROW(x->a, pom->mgmt, pom->mgmt_count, pom->mgmt_cap, mv_dep);
      pom->mgmt[pom->mgmt_count++] = *x->cur;
    } else {
      MV_GROW(x->a, pom->deps, pom->deps_count, pom->deps_cap, mv_dep);
      pom->deps[pom->deps_count++] = *x->cur;
    }
    x->cur = NULL;
  }
  x->depth--;
}

// Parse a POM document. Returns 0 on success, -1 on malformed XML.
static int mv_parse_pom(ccb_arena *a, const char *text, size_t len, mv_pom *pom) {
  mv_parser x;
  memset(&x, 0, sizeof(x));
  x.a = a;
  x.p = text;
  x.end = text + len;
  x.pom = pom;
  memset(pom, 0, sizeof(*pom));
  while (x.p < x.end && !x.failed) {
    if (*x.p == '<') {
      const char *p = x.p + 1;
      if (p + 2 < x.end && memcmp(p, "!--", 3) == 0) {
        const char *e = NULL;
        for (const char *q = p + 3; q + 2 < x.end; q++) {
          if (memcmp(q, "-->", 3) == 0) {
            e = q + 3;
            break;
          }
        }
        if (e == NULL) {
          return -1;
        }
        x.p = e;
        continue;
      }
      if (p < x.end && *p == '?') {
        const char *e = NULL;
        for (const char *q = p + 1; q + 1 < x.end; q++) {
          if (memcmp(q, "?>", 2) == 0) {
            e = q + 2;
            break;
          }
        }
        if (e == NULL) {
          return -1;
        }
        x.p = e;
        continue;
      }
      if (p < x.end && *p == '!') {
        // DOCTYPE or another declaration: skip to '>', honoring an
        // internal subset in [ ... ].
        const char *q = p + 1;
        int bracket = 0;
        while (q < x.end) {
          if (*q == '[') {
            bracket = 1;
          } else if (*q == ']') {
            bracket = 0;
          } else if (*q == '>' && !bracket) {
            break;
          }
          q++;
        }
        if (q >= x.end) {
          return -1;
        }
        x.p = q + 1;
        continue;
      }
      if (p < x.end && *p == '/') {
        const char *q = p + 1;
        char name[64];
        if (!mv_read_name(&x, &q, name, sizeof(name))) {
          return -1;
        }
        while (q < x.end && *q != '>') {
          q++;
        }
        if (q >= x.end) {
          return -1;
        }
        mv_close(&x, name);
        x.p = q + 1;
        continue;
      }
      // Open tag: read the name, skip attributes quote-aware, and
      // detect a self-closing "/>".
      const char *q = p;
      char name[64];
      if (!mv_read_name(&x, &q, name, sizeof(name))) {
        return -1;
      }
      int self_close = 0;
      while (q < x.end) {
        char ch = *q;
        if (ch == '"' || ch == '\'') {
          const char *e = memchr(q + 1, ch, (size_t)(x.end - (q + 1)));
          if (e == NULL) {
            return -1;
          }
          q = e + 1;
          continue;
        }
        if (ch == '/' && q + 1 < x.end && q[1] == '>') {
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
      if (q > x.end) {
        return -1;
      }
      mv_open(&x, name);
      if (self_close) {
        mv_close(&x, name);
      }
      x.p = q;
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
static char *mv_subst(ccb_arena *a, mv_pom *pom, const char *eff_group,
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

// ---------- fetching ----------

static char *mv_cache_dir_for(ccb_arena *a, const char *cache, const char *group,
    const char *artifact, const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, cache);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, mv_group_path(a, group));
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, version);
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

// Fetch one repository file into the cache.
// Returns 0 (usable, *final_out set), 1 (not found anywhere),
// 2 (checksum mismatch), 3 (cached file corrupt), 4 (cache-only
// miss), 5 (cannot store the download). prefetch != 0 may download;
// prefetch == 0 never touches the network.
static int mv_fetch(ccb_arena *a, const char *cache, char **repos,
    size_t repos_count, const char *group, const char *artifact,
    const char *version, const char *ext, int prefetch, int readonly,
    char **final_out) {
  char *leaf = mv_leaf(a, artifact, version, ext);
  char *final_path = mv_cache_file(a, cache, group, artifact, version, leaf);
  if (ccb_path_exists(final_path)) {
    if (mv_cache_file_ok(final_path, readonly)) {
      *final_out = final_path;
      return 0;
    }
    if (!prefetch) {
      return 3;
    }
    remove(final_path);
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, final_path);
    ccb_str_append(&s, ".ccb-sha256");
    char *side = ccb_str_steal(&s);
    remove(side);
    free(side);
  } else if (!prefetch) {
    return 4;
  }
  char *dir = mv_cache_dir_for(a, cache, group, artifact, version);
  if (ccb_mkdir_p(dir) != CCB_OK) {
    return 5;
  }
  ccb_str ts;
  ccb_str_init(&ts);
  ccb_str_append(&ts, final_path);
  ccb_str_append(&ts, ".tmp");
  char *tmp = ccb_arena_strdup(a, ts.buf);
  ccb_str_free(&ts);
  for (size_t i = 0; i < repos_count; i++) {
    char *url = mv_url(a, repos[i], group, artifact, version, leaf);
    if (ccb_net_download(url, tmp) != 0) {
      remove(tmp);
      continue;
    }
    char sha256_hex[65];
    int vr = mv_verify_downloaded(a, tmp, url, sha256_hex);
    if (vr != 0) {
      remove(tmp);
      return 2;
    }
    if (rename(tmp, final_path) != 0) {
      remove(tmp);
      return 5;
    }
    mv_write_sidecar(final_path, sha256_hex);
    *final_out = final_path;
    return 0;
  }
  return 1;
}

// ---------- resolution ----------

typedef struct {
  char *group;
  char *artifact;
  char *version;
  int depth;
} mv_qitem;

typedef struct {
  char *key;
  char *version;
} mv_seen;

static mv_seen *mv_seen_find(mv_seen *seen, size_t count, const char *key) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(seen[i].key, key) == 0) {
      return &seen[i];
    }
  }
  return NULL;
}

// Map a fetch result to a diagnostic naming the coordinate. Returns
// the status to propagate (always CCB_ERR_TOOLCHAIN).
static ccb_status mv_fetch_fail(ccb_diag_list *diags, int rc,
    const char *coord, const char *what) {
  ccb_str m;
  ccb_str_init(&m);
  ccb_str_append(&m, what);
  switch (rc) {
    case 1:
      ccb_str_append(&m, " not found in any configured repository");
      mv_diag(diags, "CCB-E3019", coord, m.buf);
      break;
    case 2:
      ccb_str_append(&m, " checksum mismatch");
      mv_diag(diags, "CCB-E3020", coord, m.buf);
      break;
    case 3:
      ccb_str_append(&m,
        " cached file failed its recorded checksum; run `ccb setup` to refresh it");
      mv_diag(diags, "CCB-E3020", coord, m.buf);
      break;
    case 4:
      ccb_str_append(&m,
        " is not in the local cache; run `ccb setup` to prefetch it");
      mv_diag(diags, "CCB-E3021", coord, m.buf);
      break;
    default:
      ccb_str_append(&m, " could not be stored in the local cache");
      mv_diag(diags, "CCB-E3019", coord, m.buf);
      break;
  }
  ccb_str_free(&m);
  return CCB_ERR_TOOLCHAIN;
}

void ccb_maven_result_free(ccb_maven_result *r) {
  if (r == NULL) {
    return;
  }
  ccb_free_str_array(r->aars, r->aars_count);
  ccb_free_str_array(r->jars, r->jars_count);
  r->aars = NULL;
  r->jars = NULL;
  r->aars_count = 0;
  r->jars_count = 0;
}

static void mv_push_result(char ***arr, size_t *count, size_t *cap,
    const char *path) {
  if (*count == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    *arr = ccb_xrealloc(*arr, ncap * sizeof(**arr));
    *cap = ncap;
  }
  (*arr)[(*count)++] = ccb_xstrdup(path);
}

ccb_status ccb_maven_resolve(const ccb_config *cfg, int prefetch, int verbose,
    int readonly, ccb_maven_result *out, ccb_diag_list *diags) {
  memset(out, 0, sizeof(*out));
  if (cfg->maven_count == 0) {
    return CCB_OK;
  }
  CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
  CCB_AUTO_FREE char *cache = ccb_maven_cache_dir();
  if (cache == NULL) {
    mv_diag(diags, "CCB-E3019", NULL,
      "no Maven cache directory available (set HOME or XDG_CACHE_HOME)");
    return CCB_ERR_TOOLCHAIN;
  }
  size_t repos_count = 0;
  char **repos = mv_repos(arena, cfg, &repos_count);

  mv_qitem *queue = NULL;
  size_t qcount = 0;
  size_t qcap = 0;
  mv_seen *seen = NULL;
  size_t seen_count = 0;
  size_t seen_cap = 0;
  size_t res_aars_cap = 0;
  size_t res_jars_cap = 0;

  // Seed the queue with the direct coordinates, in declared order.
  for (size_t i = 0; i < cfg->maven_count; i++) {
    char *g = NULL;
    char *art = NULL;
    char *v = NULL;
    if (!ccb_maven_parse_coord(cfg->maven[i], &g, &art, &v)) {
      mv_diag(diags, "CCB-E3019", cfg->maven[i], "malformed coordinate");
      free(g);
      free(art);
      free(v);
      goto fail;
    }
    ccb_str ks;
    ccb_str_init(&ks);
    ccb_str_append(&ks, g);
    ccb_str_append_c(&ks, ':');
    ccb_str_append(&ks, art);
    char *key = ccb_arena_strdup(arena, ks.buf);
    ccb_str_free(&ks);
    mv_seen *prev = mv_seen_find(seen, seen_count, key);
    if (prev != NULL) {
      if (verbose && strcmp(prev->version, v) != 0) {
        printf("maven: %s: requested %s, using nearest %s\n", key, v,
          prev->version);
      }
    } else {
      MV_GROW(arena, seen, seen_count, seen_cap, mv_seen);
      seen[seen_count].key = key;
      seen[seen_count].version = ccb_arena_strdup(arena, v);
      seen_count++;
      MV_GROW(arena, queue, qcount, qcap, mv_qitem);
      queue[qcount].group = ccb_arena_strdup(arena, g);
      queue[qcount].artifact = ccb_arena_strdup(arena, art);
      queue[qcount].version = ccb_arena_strdup(arena, v);
      queue[qcount].depth = 0;
      qcount++;
    }
    free(g);
    free(art);
    free(v);
  }

  // Breadth-first walk: the first visit to a group:artifact is the
  // nearest, so it wins and later (deeper or later-declared) versions
  // are skipped.
  size_t head = 0;
  while (head < qcount) {
    mv_qitem item = queue[head++];
    const char *coord = mv_coord_str(arena, item.group, item.artifact,
      item.version);
    char *pom_path = NULL;
    int rc = mv_fetch(arena, cache, repos, repos_count, item.group,
      item.artifact, item.version, "pom", prefetch, readonly, &pom_path);
    if (rc != 0) {
      mv_fetch_fail(diags, rc, coord, "POM");
      goto fail;
    }
    char *pom_text = NULL;
    size_t pom_len = 0;
    if (ccb_read_file(pom_path, &pom_text, &pom_len) != CCB_OK) {
      mv_diag(diags, "CCB-E3019", coord, "cached POM cannot be read");
      goto fail;
    }
    mv_pom pom;
    if (mv_parse_pom(arena, pom_text, pom_len, &pom) != 0) {
      free(pom_text);
      mv_diag(diags, "CCB-E3019", coord, "malformed POM");
      goto fail;
    }
    free(pom_text);
    // Effective coordinates: own values, else the embedded parent
    // block (the parent POM itself is never fetched in v1).
    int bad = 0;
    const char *raw_group = pom.group != NULL ? pom.group : pom.parent_group;
    const char *raw_version = pom.version != NULL ? pom.version : pom.parent_version;
    char *eff_group = mv_subst(arena, &pom, raw_group, pom.artifact,
      raw_version, raw_group, &bad);
    char *eff_artifact = mv_subst(arena, &pom, raw_group, pom.artifact,
      raw_version, pom.artifact, &bad);
    char *eff_version = mv_subst(arena, &pom, raw_group, pom.artifact,
      raw_version, raw_version, &bad);
    if (bad || eff_group == NULL || eff_artifact == NULL ||
        eff_version == NULL) {
      mv_diag(diags, "CCB-E3019", coord,
        "POM coordinates cannot be resolved (missing value or unresolvable property)");
      goto fail;
    }
    const char *packaging = "jar";
    if (pom.packaging != NULL) {
      int pbad = 0;
      char *p = mv_subst(arena, &pom, eff_group, eff_artifact, eff_version,
        pom.packaging, &pbad);
      if (pbad || p == NULL) {
        mv_diag(diags, "CCB-E3019", coord, "packaging cannot be resolved");
        goto fail;
      }
      packaging = p;
    }
    const char *ext = NULL;
    int is_aar = 0;
    if (strcmp(packaging, "aar") == 0) {
      ext = "aar";
      is_aar = 1;
    } else if (strcmp(packaging, "jar") == 0) {
      ext = "jar";
    } else if (strcmp(packaging, "pom") == 0) {
      ext = NULL;
    } else {
      ccb_str m;
      ccb_str_init(&m);
      ccb_str_append(&m, "unsupported packaging '");
      ccb_str_append(&m, packaging);
      ccb_str_append(&m, "'");
      mv_diag(diags, "CCB-E3019", coord, m.buf);
      ccb_str_free(&m);
      goto fail;
    }
    if (ext != NULL) {
      char *art_path = NULL;
      rc = mv_fetch(arena, cache, repos, repos_count, item.group,
        item.artifact, item.version, ext, prefetch, readonly, &art_path);
      if (rc != 0) {
        mv_fetch_fail(diags, rc, coord, ext);
        goto fail;
      }
      if (is_aar) {
        mv_push_result(&out->aars, &out->aars_count, &res_aars_cap, art_path);
      } else {
        mv_push_result(&out->jars, &out->jars_count, &res_jars_cap, art_path);
      }
    }
    // Transitive dependencies of this POM.
    for (size_t d = 0; d < pom.deps_count; d++) {
      mv_dep *dep = &pom.deps[d];
      int dbad = 0;
      char *scope = dep->scope != NULL ?
        mv_subst(arena, &pom, eff_group, eff_artifact, eff_version,
          dep->scope, &dbad) : (char *)"compile";
      if (dbad) {
        mv_diag(diags, "CCB-E3019", coord,
          "dependency scope cannot be resolved");
        goto fail;
      }
      if (strcmp(scope, "test") == 0 || strcmp(scope, "provided") == 0 ||
          strcmp(scope, "system") == 0 || strcmp(scope, "import") == 0) {
        continue;
      }
      char *optional = dep->optional != NULL ?
        mv_subst(arena, &pom, eff_group, eff_artifact, eff_version,
          dep->optional, &dbad) : NULL;
      if (dbad) {
        mv_diag(diags, "CCB-E3019", coord,
          "dependency optional flag cannot be resolved");
        goto fail;
      }
      if (optional != NULL && strcmp(optional, "true") == 0) {
        continue;
      }
      char *dg = mv_subst(arena, &pom, eff_group, eff_artifact, eff_version,
        dep->group, &dbad);
      char *da = mv_subst(arena, &pom, eff_group, eff_artifact, eff_version,
        dep->artifact, &dbad);
      char *dv = dep->version != NULL ?
        mv_subst(arena, &pom, eff_group, eff_artifact, eff_version,
          dep->version, &dbad) : NULL;
      if (dbad || dg == NULL || da == NULL ||
          !mv_part_ok(dg, strlen(dg)) || !mv_part_ok(da, strlen(da))) {
        mv_diag(diags, "CCB-E3019", coord,
          "dependency coordinates cannot be resolved");
        goto fail;
      }
      if (dv != NULL && !mv_part_ok(dv, strlen(dv))) {
        mv_diag(diags, "CCB-E3019", coord,
          "dependency version is path-unsafe");
        goto fail;
      }
      if (dv == NULL || dv[0] == '\0') {
        // Fall back to this POM's dependencyManagement entry.
        for (size_t m = 0; m < pom.mgmt_count; m++) {
          mv_dep *mg = &pom.mgmt[m];
          int mbad = 0;
          char *mg_g = mv_subst(arena, &pom, eff_group, eff_artifact,
            eff_version, mg->group, &mbad);
          char *mg_a = mv_subst(arena, &pom, eff_group, eff_artifact,
            eff_version, mg->artifact, &mbad);
          if (mbad || mg_g == NULL || mg_a == NULL) {
            continue;
          }
          if (strcmp(mg_g, dg) == 0 && strcmp(mg_a, da) == 0 &&
              mg->version != NULL) {
            dv = mv_subst(arena, &pom, eff_group, eff_artifact, eff_version,
              mg->version, &dbad);
            if (dbad) {
              dv = NULL;
            }
            break;
          }
        }
      }
      if (dv == NULL || dv[0] == '\0') {
        ccb_str m;
        ccb_str_init(&m);
        ccb_str_append(&m, "dependency ");
        ccb_str_append(&m, dg);
        ccb_str_append_c(&m, ':');
        ccb_str_append(&m, da);
        ccb_str_append(&m,
          " has no version and no dependencyManagement entry");
        mv_diag(diags, "CCB-E3019", coord, m.buf);
        ccb_str_free(&m);
        goto fail;
      }
      ccb_str ks;
      ccb_str_init(&ks);
      ccb_str_append(&ks, dg);
      ccb_str_append_c(&ks, ':');
      ccb_str_append(&ks, da);
      char *key = ccb_arena_strdup(arena, ks.buf);
      ccb_str_free(&ks);
      mv_seen *prev = mv_seen_find(seen, seen_count, key);
      if (prev != NULL) {
        if (verbose && strcmp(prev->version, dv) != 0) {
          printf("maven: %s: requested %s, using nearest %s\n", key, dv,
            prev->version);
        }
        continue;
      }
      MV_GROW(arena, seen, seen_count, seen_cap, mv_seen);
      seen[seen_count].key = key;
      seen[seen_count].version = ccb_arena_strdup(arena, dv);
      seen_count++;
      MV_GROW(arena, queue, qcount, qcap, mv_qitem);
      queue[qcount].group = dg;
      queue[qcount].artifact = da;
      queue[qcount].version = dv;
      queue[qcount].depth = item.depth + 1;
      qcount++;
    }
  }
  return CCB_OK;

fail:
  ccb_maven_result_free(out);
  memset(out, 0, sizeof(*out));
  return CCB_ERR_TOOLCHAIN;
}
