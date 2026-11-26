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

// One repository: a normalized base URL plus optional Basic-auth
// credentials. The password lives only in arena memory and travels
// only in a curl argv; it never appears in a URL, a log line, or a
// diagnostic.
typedef struct {
  char *base;
  char *user; // NULL: no auth
  char *pass; // NULL when user is NULL
} mv_repo;

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

// Parse one [deps] maven_repos entry ("url" or
// "url username password-source") into *repo. Returns 0 on success,
// -1 with a diagnostic on a missing environment variable for an
// "env:VAR" password source. Config load validates the shape, so a
// malformed entry here is skipped by the caller; only the env
// failure is a resolve-time error, because the variable is only
// read when the repository is actually used.
static int mv_parse_repo_entry(ccb_arena *a, const char *entry,
    mv_repo *repo, ccb_diag_list *diags) {
  const char *sp1 = strchr(entry, ' ');
  size_t url_len = sp1 != NULL ? (size_t)(sp1 - entry) : strlen(entry);
  char *raw = ccb_arena_strndup(a, entry, url_len);
  repo->base = mv_norm_base(a, raw);
  repo->user = NULL;
  repo->pass = NULL;
  if (sp1 == NULL) {
    return 0;
  }
  const char *user = sp1 + 1;
  const char *sp2 = strchr(user, ' ');
  repo->user = ccb_arena_strndup(a, user, (size_t)(sp2 - user));
  const char *pw = sp2 + 1;
  if (memcmp(pw, "env:", 4) == 0) {
    const char *val = getenv(pw + 4);
    if (val == NULL) {
      ccb_str m;
      ccb_str_init(&m);
      ccb_str_append(&m, "repository ");
      ccb_str_append(&m, repo->base);
      ccb_str_append(&m,
        " needs credentials from environment variable ");
      ccb_str_append(&m, pw + 4);
      ccb_str_append(&m, ", which is not set");
      mv_diag(diags, "CCB-E3019", NULL, m.buf);
      ccb_str_free(&m);
      return -1;
    }
    repo->pass = ccb_arena_strdup(a, val);
    return 0;
  }
  repo->pass = ccb_arena_strdup(a, pw);
  return 0;
}

// Repository list: CCB_MAVEN_REPOS (comma-separated, replaces the
// defaults; the test seam for fixture file:// repos) or Google Maven
// plus Maven Central when the project opts in, plus the project's
// [deps] maven_repos entries in declared order. Returns NULL with a
// diagnostic on a missing credential environment variable.
static mv_repo *mv_repos(ccb_arena *a, const ccb_config *cfg,
    size_t *count, ccb_diag_list *diags) {
  mv_repo *repos = NULL;
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
        MV_GROW(a, repos, n, cap, mv_repo);
        repos[n].base = mv_norm_base(a, raw);
        repos[n].user = NULL;
        repos[n].pass = NULL;
        n++;
      }
      p = comma != NULL ? comma + 1 : p + len;
    }
  }
  if (n == 0) {
    MV_GROW(a, repos, n, cap, mv_repo);
    repos[n].base = mv_norm_base(a, MV_GOOGLE_MAVEN);
    repos[n].user = NULL;
    repos[n].pass = NULL;
    n++;
    if (cfg->maven_central) {
      MV_GROW(a, repos, n, cap, mv_repo);
      repos[n].base = mv_norm_base(a, MV_MAVEN_CENTRAL);
      repos[n].user = NULL;
      repos[n].pass = NULL;
      n++;
    }
    for (size_t i = 0; i < cfg->maven_repos_count; i++) {
      // A malformed entry is skipped: config load validates the
      // list, so resolve never sees one through a real config.
      const char *e = cfg->maven_repos[i];
      const char *sp1 = strchr(e, ' ');
      if (sp1 != NULL && strchr(sp1 + 1, ' ') == NULL) {
        continue;
      }
      MV_GROW(a, repos, n, cap, mv_repo);
      if (mv_parse_repo_entry(a, e, &repos[n], diags) != 0) {
        return NULL;
      }
      n++;
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
    const char *url, const char *user, const char *pass,
    char *sha256_hex) {
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
    int dl = ccb_net_download_auth(curl_url, chk_path, user, pass);
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

// One <profile>: id, <activeByDefault> activation, and the
// properties, dependencies, and dependencyManagement it contributes
// when active. Other activation forms (jdk, os, property, file)
// are parsed nowhere: a profile using them is inactive unless its
// id is listed in [deps] maven_profiles.
typedef struct {
  char *id;
  int active_default;
  mv_prop *props;
  size_t props_count;
  size_t props_cap;
  mv_dep *deps;
  size_t deps_count;
  size_t deps_cap;
  mv_dep *mgmt;
  size_t mgmt_count;
  size_t mgmt_cap;
} mv_profile;

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
  mv_profile *profiles;
  size_t profiles_count;
  size_t profiles_cap;
} mv_pom;

typedef struct {
  ccb_arena *a;
  const char *p;
  const char *end;
  char stack[32][64];
  int depth;
  mv_pom *pom;
  mv_dep *cur;
  // Which list the open <dependency> belongs to: 0 plain
  // dependencies, 1 dependencyManagement, 2 profile dependencies,
  // 3 profile dependencyManagement.
  int cur_where;
  mv_profile *cur_profile;
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
static int mv_fetch(ccb_arena *a, const char *cache, mv_repo *repos,
    size_t repos_count, const char *group, const char *artifact,
    const char *version, const char *dir_version, const char *ext,
    int prefetch, int readonly, char **final_out) {
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
    // The remote directory is the declared version: a snapshot's
    // timestamped files live under its -SNAPSHOT directory, while
    // the leaf and the cache path use the concrete version.
    char *url = mv_url(a, repos[i].base, group, artifact, dir_version,
      leaf);
    if (ccb_net_download_auth(url, tmp, repos[i].user, repos[i].pass) != 0) {
      remove(tmp);
      continue;
    }
    char sha256_hex[65];
    int vr = mv_verify_downloaded(a, tmp, url, repos[i].user,
      repos[i].pass, sha256_hex);
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

// Forward: mv_fetch_fail is defined in the resolution section
// below; the metadata helpers need it for missing/stale metadata.
static ccb_status mv_fetch_fail(ccb_diag_list *diags, int rc,
    const char *coord, const char *what);

// ---------- version metadata, ranges, snapshots ----------
//
// Version specs beyond an exact version resolve through
// maven-metadata.xml: artifact-level metadata lists every published
// version plus <latest>/<release>; snapshot metadata under a
// -SNAPSHOT version directory carries the timestamp/buildNumber of
// the newest snapshot build. Prefetch (setup) always re-downloads
// metadata fresh, so ranges and snapshots float; cache-only builds
// use the cached copy, so a build never downloads and a range pins
// to whatever setup last resolved (the lockfile records the concrete
// version). A range or snapshot that resolves to a concrete version
// behaves exactly like an exact version from there on: cache layout,
// checksums, and the lockfile all key on the concrete version.

// Text of the first <tag>...</tag> in an XML document, trimmed, as
// an arena string; NULL when the tag is absent or empty.
static char *mv_xml_tag(ccb_arena *a, const char *text, size_t len,
    const char *tag) {
  ccb_str open;
  ccb_str_init(&open);
  ccb_str_append_c(&open, '<');
  ccb_str_append(&open, tag);
  ccb_str_append_c(&open, '>');
  ccb_str close;
  ccb_str_init(&close);
  ccb_str_append(&close, "</");
  ccb_str_append(&close, tag);
  ccb_str_append_c(&close, '>');
  const char *start = NULL;
  const char *end = text + len;
  for (const char *p = text; p + open.len <= end; p++) {
    if (memcmp(p, open.buf, open.len) == 0) {
      start = p + open.len;
      break;
    }
  }
  char *out = NULL;
  if (start != NULL) {
    const char *stop = NULL;
    for (const char *p = start; p + close.len <= end; p++) {
      if (memcmp(p, close.buf, close.len) == 0) {
        stop = p;
        break;
      }
    }
    if (stop != NULL && stop > start) {
      char *raw = ccb_arena_strndup(a, start, (size_t)(stop - start));
      char *trimmed = mv_trim(raw);
      if (trimmed[0] != '\0') {
        out = trimmed;
      }
    }
  }
  ccb_str_free(&open);
  ccb_str_free(&close);
  return out;
}

// Every <tag>...</tag> text in a document, in order (arena strings).
static void mv_xml_tag_all(ccb_arena *a, const char *text, size_t len,
    const char *tag, char ***out, size_t *count) {
  ccb_str open;
  ccb_str_init(&open);
  ccb_str_append_c(&open, '<');
  ccb_str_append(&open, tag);
  ccb_str_append_c(&open, '>');
  ccb_str close;
  ccb_str_init(&close);
  ccb_str_append(&close, "</");
  ccb_str_append(&close, tag);
  ccb_str_append_c(&close, '>');
  char **vals = NULL;
  size_t n = 0;
  size_t cap = 0;
  const char *end = text + len;
  const char *p = text;
  while (p + open.len <= end) {
    if (memcmp(p, open.buf, open.len) != 0) {
      p++;
      continue;
    }
    const char *start = p + open.len;
    const char *stop = NULL;
    for (const char *q = start; q + close.len <= end; q++) {
      if (memcmp(q, close.buf, close.len) == 0) {
        stop = q;
        break;
      }
    }
    if (stop == NULL) {
      break;
    }
    if (stop > start) {
      char *raw = ccb_arena_strndup(a, start, (size_t)(stop - start));
      char *trimmed = mv_trim(raw);
      if (trimmed[0] != '\0') {
        MV_GROW(a, vals, n, cap, char *);
        vals[n++] = trimmed;
      }
    }
    p = stop + close.len;
  }
  ccb_str_free(&open);
  ccb_str_free(&close);
  *out = vals;
  *count = n;
}

static char *mv_metadata_path(ccb_arena *a, const char *cache,
    const char *group, const char *artifact, const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, cache);
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, mv_group_path(a, group));
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '/');
  if (version != NULL) {
    ccb_str_append(&s, version);
    ccb_str_append_c(&s, '/');
  }
  ccb_str_append(&s, "maven-metadata.xml");
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

static char *mv_metadata_url(ccb_arena *a, const char *base,
    const char *group, const char *artifact, const char *version) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, base);
  ccb_str_append(&s, mv_group_path(a, group));
  ccb_str_append_c(&s, '/');
  ccb_str_append(&s, artifact);
  ccb_str_append_c(&s, '/');
  if (version != NULL) {
    ccb_str_append(&s, version);
    ccb_str_append_c(&s, '/');
  }
  ccb_str_append(&s, "maven-metadata.xml");
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

// Fetch maven-metadata.xml for an artifact (version == NULL) or for
// a snapshot version directory. Return codes mirror mv_fetch: 0
// usable (*path_out set), 1 not found anywhere, 3 cached file
// corrupt, 4 cache-only miss, 5 cannot store. Prefetch always
// re-downloads fresh (ranges and snapshots float at setup time);
// cache-only never touches the network.
static int mv_fetch_metadata(ccb_arena *a, const char *cache,
    mv_repo *repos, size_t repos_count, const char *group,
    const char *artifact, const char *version, int prefetch,
    int readonly, char **path_out) {
  char *final_path = mv_metadata_path(a, cache, group, artifact, version);
  if (!prefetch) {
    if (!ccb_path_exists(final_path)) {
      return 4;
    }
    if (!mv_cache_file_ok(final_path, readonly)) {
      return 3;
    }
    *path_out = final_path;
    return 0;
  }
  char *dir = ccb_dirname(final_path);
  if (ccb_mkdir_p(dir) != CCB_OK) {
    free(dir);
    return 5;
  }
  free(dir);
  ccb_str ts;
  ccb_str_init(&ts);
  ccb_str_append(&ts, final_path);
  ccb_str_append(&ts, ".tmp");
  char *tmp = ccb_arena_strdup(a, ts.buf);
  ccb_str_free(&ts);
  for (size_t i = 0; i < repos_count; i++) {
    char *url = mv_metadata_url(a, repos[i].base, group, artifact,
      version);
    if (ccb_net_download_auth(url, tmp, repos[i].user,
        repos[i].pass) != 0) {
      remove(tmp);
      continue;
    }
    char hex[65];
    if (ccb_sha256_file(tmp, hex) != CCB_OK) {
      remove(tmp);
      return 5;
    }
    if (rename(tmp, final_path) != 0) {
      remove(tmp);
      return 5;
    }
    mv_write_sidecar(final_path, hex);
    *path_out = final_path;
    return 0;
  }
  return 1;
}

// ---------- Maven version ordering ----------
//
// ComparableVersion rules, simplified to the cases real repositories
// publish: dot/dash-separated tokens, digit runs compared numerically
// (arbitrary precision via length-then-lexical), qualifier order
// alpha < beta < milestone < rc < snapshot < "" (release) < sp, and
// unknown qualifiers sorting after sp, lexically. "1.0" == "1.0.0";
// "1.0-alpha" < "1.0"; "1.0-SNAPSHOT" < "1.0" < "1.0-sp-1".

typedef struct {
  int is_num; // 1: digit run, 0: qualifier string
  const char *text;
  size_t len;
} mv_vtok;

// Read one token starting at *pp (past '.'/'-' separators). Returns
// 0 when no token remains.
static int mv_next_vtok(const char **pp, mv_vtok *tok) {
  const char *p = *pp;
  while (*p == '.' || *p == '-') {
    p++;
  }
  if (*p == '\0') {
    return 0;
  }
  const char *start = p;
  if (*p >= '0' && *p <= '9') {
    while (*p >= '0' && *p <= '9') {
      p++;
    }
    tok->is_num = 1;
  } else {
    while (*p != '\0' && *p != '.' && *p != '-' &&
        !(*p >= '0' && *p <= '9')) {
      p++;
    }
    tok->is_num = 0;
  }
  tok->text = start;
  tok->len = (size_t)(p - start);
  *pp = p;
  return 1;
}

// Rank of a qualifier token (case-insensitive): 1 alpha, 2 beta,
// 3 milestone, 4 rc, 5 snapshot, 6 release ("" and the ga/final/
// release aliases), 7 sp, 8 unknown.
static int mv_qual_rank(const char *s, size_t n) {
  static const struct { const char *name; int rank; } known[] = {
    { "alpha", 1 }, { "beta", 2 }, { "milestone", 3 }, { "rc", 4 },
    { "cr", 4 }, { "snapshot", 5 }, { "", 6 }, { "ga", 6 },
    { "final", 6 }, { "release", 6 }, { "sp", 7 },
  };
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    size_t kn = strlen(known[i].name);
    if (kn != n) {
      continue;
    }
    size_t k = 0;
    while (k < n && tolower((unsigned char)s[k]) ==
        known[i].name[k]) {
      k++;
    }
    if (k == n) {
      return known[i].rank;
    }
  }
  return 8;
}

// Case-insensitive lexical compare of two qualifier tokens.
static int mv_qual_lex(const char *a, size_t an, const char *b,
    size_t bn) {
  size_t n = an < bn ? an : bn;
  for (size_t i = 0; i < n; i++) {
    int ca = tolower((unsigned char)a[i]);
    int cb = tolower((unsigned char)b[i]);
    if (ca != cb) {
      return ca < cb ? -1 : 1;
    }
  }
  if (an == bn) {
    return 0;
  }
  return an < bn ? -1 : 1;
}

// Compare two digit runs numerically: strip leading zeros, longer
// wins, else lexical.
static int mv_num_cmp(const char *a, size_t an, const char *b,
    size_t bn) {
  while (an > 0 && *a == '0') {
    a++;
    an--;
  }
  while (bn > 0 && *b == '0') {
    b++;
    bn--;
  }
  if (an != bn) {
    return an < bn ? -1 : 1;
  }
  for (size_t i = 0; i < an; i++) {
    if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }
  return 0;
}

static int mv_version_cmp(const char *a, const char *b) {
  const char *pa = a;
  const char *pb = b;
  for (;;) {
    mv_vtok ta;
    mv_vtok tb;
    int ha = mv_next_vtok(&pa, &ta);
    int hb = mv_next_vtok(&pb, &tb);
    if (!ha && !hb) {
      return 0;
    }
    if (!ha) {
      // a ran out: the missing token is 0 when b has a number,
      // "" (release) when b has a qualifier. A qualifier with the
      // release rank ("" or a ga/final/release alias) equals it.
      if (tb.is_num) {
        return mv_num_cmp("0", 1, tb.text, tb.len);
      }
      int rb = mv_qual_rank(tb.text, tb.len);
      if (mv_qual_rank("", 0) == rb) {
        return 0;
      }
      return mv_qual_rank("", 0) < rb ? -1 : 1;
    }
    if (!hb) {
      if (ta.is_num) {
        return mv_num_cmp(ta.text, ta.len, "0", 1);
      }
      int ra = mv_qual_rank(ta.text, ta.len);
      if (ra == mv_qual_rank("", 0)) {
        return 0;
      }
      return ra < mv_qual_rank("", 0) ? -1 : 1;
    }
    if (ta.is_num && tb.is_num) {
      int c = mv_num_cmp(ta.text, ta.len, tb.text, tb.len);
      if (c != 0) {
        return c;
      }
      continue;
    }
    if (ta.is_num != tb.is_num) {
      // A number sorts after a qualifier.
      return ta.is_num ? 1 : -1;
    }
    int ra = mv_qual_rank(ta.text, ta.len);
    int rb = mv_qual_rank(tb.text, tb.len);
    if (ra != rb) {
      return ra < rb ? -1 : 1;
    }
    if (ra == 8) {
      int c = mv_qual_lex(ta.text, ta.len, tb.text, tb.len);
      if (c != 0) {
        return c;
      }
    }
  }
}

// ---------- version ranges ----------

typedef struct {
  char *low; // NULL: unbounded below
  int low_inc;
  char *high; // NULL: unbounded above
  int high_inc;
} mv_range;

// Parse a range spec ("[1.0,2.0)", "[1.0]", "[1.0,)", "(,2.0]",
// "[1.0,2.0),[3.0,4.0)") into arena ranges. Returns 0 on success,
// -1 on malformed input.
static int mv_parse_ranges(ccb_arena *a, const char *spec,
    mv_range **out, size_t *count) {
  mv_range *ranges = NULL;
  size_t n = 0;
  size_t cap = 0;
  const char *p = spec;
  while (*p != '\0') {
    while (*p == ' ' || *p == ',') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    if (*p != '[' && *p != '(') {
      return -1;
    }
    int low_inc = *p == '[';
    p++;
    const char *close = p;
    while (*close != '\0' && *close != ']' && *close != ')') {
      close++;
    }
    if (*close == '\0') {
      return -1;
    }
    int high_inc = *close == ']';
    const char *comma = memchr(p, ',', (size_t)(close - p));
    char *low = NULL;
    char *high = NULL;
    if (comma == NULL) {
      // "[1.0]": exactly one version.
      char *v = ccb_arena_strndup(a, p, (size_t)(close - p));
      char *t = mv_trim(v);
      if (t[0] == '\0' || !low_inc || !high_inc) {
        return -1;
      }
      low = t;
      high = t;
    } else {
      char *l = ccb_arena_strndup(a, p, (size_t)(comma - p));
      char *h = ccb_arena_strndup(a, comma + 1,
        (size_t)(close - (comma + 1)));
      char *lt = mv_trim(l);
      char *ht = mv_trim(h);
      if (lt[0] != '\0') {
        low = lt;
      }
      if (ht[0] != '\0') {
        high = ht;
      }
    }
    MV_GROW(a, ranges, n, cap, mv_range);
    ranges[n].low = low;
    ranges[n].low_inc = low_inc;
    ranges[n].high = high;
    ranges[n].high_inc = high_inc;
    n++;
    p = close + 1;
  }
  if (n == 0) {
    return -1;
  }
  *out = ranges;
  *count = n;
  return 0;
}

static int mv_version_in_ranges(const char *v, mv_range *ranges,
    size_t count) {
  for (size_t i = 0; i < count; i++) {
    int ok = 1;
    if (ranges[i].low != NULL) {
      int c = mv_version_cmp(v, ranges[i].low);
      if (c < 0 || (c == 0 && !ranges[i].low_inc)) {
        ok = 0;
      }
    }
    if (ok && ranges[i].high != NULL) {
      int c = mv_version_cmp(v, ranges[i].high);
      if (c > 0 || (c == 0 && !ranges[i].high_inc)) {
        ok = 0;
      }
    }
    if (ok) {
      return 1;
    }
  }
  return 0;
}

static int mv_is_range_spec(const char *s) {
  return s[0] == '[' || s[0] == '(';
}

static int mv_is_snapshot_spec(const char *s) {
  size_t n = strlen(s);
  return n >= 9 && memcmp(s + n - 9, "-SNAPSHOT", 9) == 0;
}

// A snapshot timestamp/buildNumber is digits and dots only, so it
// can never smuggle a path separator into a cache path or URL.
static int mv_snapshot_part_ok(const char *s) {
  if (s[0] == '\0') {
    return 0;
  }
  for (const char *p = s; *p != '\0'; p++) {
    if (!(*p >= '0' && *p <= '9') && *p != '.') {
      return 0;
    }
  }
  return 1;
}

// Resolve a version spec to a concrete version (arena string), or
// NULL with a diagnostic. Exact versions pass through unchanged;
// ranges, LATEST, and RELEASE read the artifact-level metadata;
// -SNAPSHOT versions read the snapshot metadata and resolve to the
// newest timestamped build.
static char *mv_resolve_version_ctx(ccb_arena *a, const char *cache,
    mv_repo *repos, size_t repos_count, int prefetch, int readonly,
    ccb_diag_list *diags, const char *group, const char *artifact,
    const char *spec) {
  if (!mv_is_range_spec(spec) && strcmp(spec, "LATEST") != 0 &&
      strcmp(spec, "RELEASE") != 0 && !mv_is_snapshot_spec(spec)) {
    return ccb_arena_strdup(a, spec);
  }
  if (mv_is_snapshot_spec(spec)) {
    char *meta_path = NULL;
    int rc = mv_fetch_metadata(a, cache, repos, repos_count, group,
      artifact, spec, prefetch, readonly, &meta_path);
    if (rc != 0) {
      char *coord = mv_coord_str(a, group, artifact, spec);
      mv_fetch_fail(diags, rc, coord, "snapshot metadata");
      return NULL;
    }
    char *text = NULL;
    size_t len = 0;
    char *coord = mv_coord_str(a, group, artifact, spec);
    if (ccb_read_file(meta_path, &text, &len) != CCB_OK) {
      mv_diag(diags, "CCB-E3019", coord,
        "cached snapshot metadata cannot be read");
      return NULL;
    }
    char *ts = mv_xml_tag(a, text, len, "timestamp");
    char *bn = mv_xml_tag(a, text, len, "buildNumber");
    free(text);
    if (ts == NULL || bn == NULL || !mv_snapshot_part_ok(ts) ||
        !mv_snapshot_part_ok(bn)) {
      mv_diag(diags, "CCB-E3019", coord,
        "snapshot metadata has no usable timestamp/buildNumber");
      return NULL;
    }
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append_n(&s, spec, strlen(spec) - 9);
    ccb_str_append_c(&s, '-');
    ccb_str_append(&s, ts);
    ccb_str_append_c(&s, '-');
    ccb_str_append(&s, bn);
    char *concrete = ccb_arena_strdup(a, s.buf);
    ccb_str_free(&s);
    return concrete;
  }
  char *meta_path = NULL;
  int rc = mv_fetch_metadata(a, cache, repos, repos_count, group,
    artifact, NULL, prefetch, readonly, &meta_path);
  if (rc != 0) {
    char *coord = mv_coord_str(a, group, artifact, spec);
    mv_fetch_fail(diags, rc, coord, "maven metadata");
    return NULL;
  }
  char *text = NULL;
  size_t len = 0;
  char *coord = mv_coord_str(a, group, artifact, spec);
  if (ccb_read_file(meta_path, &text, &len) != CCB_OK) {
    mv_diag(diags, "CCB-E3019", coord, "cached maven metadata cannot be read");
    return NULL;
  }
  if (strcmp(spec, "LATEST") == 0 || strcmp(spec, "RELEASE") == 0) {
    const char *tag = strcmp(spec, "LATEST") == 0 ? "latest" : "release";
    char *v = mv_xml_tag(a, text, len, tag);
    free(text);
    if (v == NULL || !mv_part_ok(v, strlen(v))) {
      ccb_str m;
      ccb_str_init(&m);
      ccb_str_append(&m, "maven metadata has no <");
      ccb_str_append(&m, tag);
      ccb_str_append(&m, "> version");
      mv_diag(diags, "CCB-E3019", coord, m.buf);
      ccb_str_free(&m);
      return NULL;
    }
    return v;
  }
  mv_range *ranges = NULL;
  size_t ranges_count = 0;
  if (mv_parse_ranges(a, spec, &ranges, &ranges_count) != 0) {
    free(text);
    mv_diag(diags, "CCB-E3019", coord, "malformed version range");
    return NULL;
  }
  char **versions = NULL;
  size_t versions_count = 0;
  mv_xml_tag_all(a, text, len, "version", &versions, &versions_count);
  free(text);
  char *best = NULL;
  for (size_t i = 0; i < versions_count; i++) {
    if (!mv_part_ok(versions[i], strlen(versions[i]))) {
      continue;
    }
    if (mv_version_in_ranges(versions[i], ranges, ranges_count) &&
        (best == NULL ||
          mv_version_cmp(versions[i], best) > 0)) {
      best = versions[i];
    }
  }
  if (best == NULL) {
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_append(&m, "no published version of ");
    ccb_str_append(&m, group);
    ccb_str_append_c(&m, ':');
    ccb_str_append(&m, artifact);
    ccb_str_append(&m, " matches range ");
    ccb_str_append(&m, spec);
    mv_diag(diags, "CCB-E3019", coord, m.buf);
    ccb_str_free(&m);
    return NULL;
  }
  return best;
}

// ---------- resolution ----------

typedef struct {
  char *group;
  char *artifact;
  char *version;
  // Repository directory version: the declared version for a
  // snapshot (its timestamped files live under the -SNAPSHOT
  // directory), else the same as version.
  char *dir_version;
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

void ccb_maven_entries_free(ccb_maven_entry *entries, size_t count) {
  if (entries == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(entries[i].group);
    free(entries[i].artifact);
    free(entries[i].version);
    free(entries[i].packaging);
    free(entries[i].sha256);
    free(entries[i].path);
  }
  free(entries);
}

void ccb_maven_result_free(ccb_maven_result *r) {
  if (r == NULL) {
    return;
  }
  ccb_free_str_array(r->aars, r->aars_count);
  ccb_free_str_array(r->jars, r->jars_count);
  ccb_maven_entries_free(r->entries, r->entries_count);
  r->aars = NULL;
  r->jars = NULL;
  r->entries = NULL;
  r->aars_count = 0;
  r->jars_count = 0;
  r->entries_count = 0;
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

static void mv_push_entry(ccb_maven_result *out, size_t *cap,
    const char *group, const char *artifact, const char *version,
    const char *packaging, const char *sha256, const char *path) {
  if (out->entries_count == *cap) {
    size_t ncap = *cap == 0 ? 8 : *cap * 2;
    out->entries = ccb_xrealloc(out->entries, ncap * sizeof(*out->entries));
    *cap = ncap;
  }
  ccb_maven_entry *e = &out->entries[out->entries_count++];
  e->group = ccb_xstrdup(group);
  e->artifact = ccb_xstrdup(artifact);
  e->version = ccb_xstrdup(version);
  e->packaging = ccb_xstrdup(packaging);
  e->sha256 = ccb_xstrdup(sha256);
  e->path = ccb_xstrdup(path);
}

// ---------- exclusions ----------

typedef struct {
  char *group;
  char *artifact;
  char *version; // NULL: every version of the artifact is excluded
} mv_excl;

// Parse cfg->maven_exclude into arena entries. Malformed entries are
// skipped: config load validates the list, so resolve never sees one
// through a real config; a hand-built config just loses that entry.
static mv_excl *mv_exclusions(ccb_arena *a, const ccb_config *cfg,
    size_t *count) {
  mv_excl *excls = NULL;
  size_t n = 0;
  size_t cap = 0;
  for (size_t i = 0; i < cfg->maven_exclude_count; i++) {
    const char *s = cfg->maven_exclude[i];
    const char *c1 = strchr(s, ':');
    if (c1 == NULL) {
      continue;
    }
    const char *c2 = strchr(c1 + 1, ':');
    if (c2 != NULL && strchr(c2 + 1, ':') != NULL) {
      continue;
    }
    MV_GROW(a, excls, n, cap, mv_excl);
    excls[n].group = ccb_arena_strndup(a, s, (size_t)(c1 - s));
    excls[n].artifact = ccb_arena_strndup(a, c1 + 1,
      c2 != NULL ? (size_t)(c2 - (c1 + 1)) : strlen(c1 + 1));
    excls[n].version = c2 != NULL ? ccb_arena_strdup(a, c2 + 1) : NULL;
    n++;
  }
  *count = n;
  return excls;
}

static int mv_is_excluded(mv_excl *excls, size_t count, const char *group,
    const char *artifact, const char *version) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(excls[i].group, group) == 0 &&
        strcmp(excls[i].artifact, artifact) == 0 &&
        (excls[i].version == NULL ||
          strcmp(excls[i].version, version) == 0)) {
      return 1;
    }
  }
  return 0;
}

// ---------- lockfile (ccb.lock) ----------
//
// One coordinate per line, sorted by group:artifact:
//   com.example:lib:1.0 aar <sha256hex>
// The file is generated by `ccb setup` after a successful prefetch
// and enforced by cache-only resolution (check/build). Comment lines
// (#) and blank lines are ignored on load.

typedef struct {
  char *group;
  char *artifact;
  char *version;
  char *packaging;
  char *sha256;
} mv_lock_entry;

static mv_lock_entry *mv_lock_find(mv_lock_entry *ents, size_t count,
    const char *group, const char *artifact) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(ents[i].group, group) == 0 &&
        strcmp(ents[i].artifact, artifact) == 0) {
      return &ents[i];
    }
  }
  return NULL;
}

static int mv_hex64_ok(const char *s) {
  if (strlen(s) != 64) {
    return 0;
  }
  for (int i = 0; i < 64; i++) {
    if (!isxdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}

// Push the "malformed line N in ccb.lock" diagnostic. Returns -1.
static int mv_lock_bad(ccb_diag_list *diags, size_t line_no) {
  ccb_str m;
  ccb_str_init(&m);
  ccb_str_appendf(&m, "malformed line %zu in ccb.lock", line_no);
  mv_diag(diags, "CCB-E3023", "ccb.lock", m.buf);
  ccb_str_free(&m);
  return -1;
}

// Parse one non-blank, non-comment lockfile line (p..le) and append
// it to *ents. Returns 0 on success, -1 with a diagnostic on
// malformed content.
static int mv_lock_line(ccb_arena *a, mv_lock_entry **ents, size_t *n,
    size_t *cap, const char *p, const char *le, size_t line_no,
    ccb_diag_list *diags) {
  // Fields: coord, packaging, sha256, separated by spaces.
  const char *s1 = memchr(p, ' ', (size_t)(le - p));
  const char *s2 = s1 != NULL ?
    memchr(s1 + 1, ' ', (size_t)(le - (s1 + 1))) : NULL;
  if (s1 == NULL || s2 == NULL || s2 + 1 >= le) {
    return mv_lock_bad(diags, line_no);
  }
  const char *c1 = memchr(p, ':', (size_t)(s1 - p));
  const char *c2 = c1 != NULL ?
    memchr(c1 + 1, ':', (size_t)(s1 - (c1 + 1))) : NULL;
  if (c1 == NULL || c2 == NULL ||
      !mv_part_ok(p, (size_t)(c1 - p)) ||
      !mv_part_ok(c1 + 1, (size_t)(c2 - (c1 + 1))) ||
      !mv_part_ok(c2 + 1, (size_t)(s1 - (c2 + 1)))) {
    return mv_lock_bad(diags, line_no);
  }
  char *g = ccb_arena_strndup(a, p, (size_t)(c1 - p));
  char *art = ccb_arena_strndup(a, c1 + 1, (size_t)(c2 - (c1 + 1)));
  char *v = ccb_arena_strndup(a, c2 + 1, (size_t)(s1 - (c2 + 1)));
  char *pack = ccb_arena_strndup(a, s1 + 1, (size_t)(s2 - (s1 + 1)));
  char *sha = ccb_arena_strndup(a, s2 + 1, (size_t)(le - (s2 + 1)));
  if ((strcmp(pack, "aar") != 0 && strcmp(pack, "jar") != 0 &&
       strcmp(pack, "pom") != 0) || !mv_hex64_ok(sha) ||
      mv_lock_find(*ents, *n, g, art) != NULL) {
    return mv_lock_bad(diags, line_no);
  }
  MV_GROW(a, *ents, *n, *cap, mv_lock_entry);
  (*ents)[*n].group = g;
  (*ents)[*n].artifact = art;
  (*ents)[*n].version = v;
  (*ents)[*n].packaging = pack;
  (*ents)[*n].sha256 = sha;
  (*n)++;
  return 0;
}

// Load ccb.lock. Returns 0 on success (entries arena-allocated),
// -1 with a CCB-E3023 diagnostic on any malformed content.
static int mv_lock_load(ccb_arena *a, const char *path,
    mv_lock_entry **out, size_t *out_count, ccb_diag_list *diags) {
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &text, &len) != CCB_OK) {
    mv_diag(diags, "CCB-E3023", "ccb.lock", "lockfile cannot be read");
    return -1;
  }
  mv_lock_entry *ents = NULL;
  size_t n = 0;
  size_t cap = 0;
  size_t line_no = 0;
  const char *p = text;
  const char *end = text + len;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *line_end = nl != NULL ? nl : end;
    line_no++;
    const char *le = line_end;
    while (le > p && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) {
      le--;
    }
    if (le > p && p[0] != '#') {
      if (mv_lock_line(a, &ents, &n, &cap, p, le, line_no, diags) != 0) {
        free(text);
        return -1;
      }
    }
    p = nl != NULL ? nl + 1 : end;
  }
  free(text);
  *out = ents;
  *out_count = n;
  return 0;
}

// Enforce a present lockfile against the resolved tree. Returns 0
// when the tree matches exactly, -1 with a CCB-E3023 diagnostic on
// the first mismatch found.
static int mv_lock_enforce(ccb_arena *a, const char *lock_path,
    const ccb_maven_result *resolved, ccb_diag_list *diags) {
  mv_lock_entry *locked = NULL;
  size_t locked_count = 0;
  if (mv_lock_load(a, lock_path, &locked, &locked_count, diags) != 0) {
    return -1;
  }
  for (size_t i = 0; i < resolved->entries_count; i++) {
    ccb_maven_entry *e = &resolved->entries[i];
    mv_lock_entry *l = mv_lock_find(locked, locked_count, e->group,
      e->artifact);
    char *coord = mv_coord_str(a, e->group, e->artifact, e->version);
    if (l == NULL) {
      mv_diag(diags, "CCB-E3023", coord,
        "is not in ccb.lock; run `ccb setup` to update the lockfile");
      return -1;
    }
    if (strcmp(l->version, e->version) != 0) {
      ccb_str m;
      ccb_str_init(&m);
      ccb_str_append(&m, "is locked at ");
      ccb_str_append(&m, l->version);
      ccb_str_append(&m, " but resolved ");
      ccb_str_append(&m, e->version);
      ccb_str_append(&m, "; run `ccb setup` to update the lockfile");
      mv_diag(diags, "CCB-E3023", coord, m.buf);
      ccb_str_free(&m);
      return -1;
    }
    if (strcmp(l->packaging, e->packaging) != 0) {
      ccb_str m;
      ccb_str_init(&m);
      ccb_str_append(&m, "packaging changed from ");
      ccb_str_append(&m, l->packaging);
      ccb_str_append(&m, " to ");
      ccb_str_append(&m, e->packaging);
      ccb_str_append(&m, " since ccb.lock; run `ccb setup` to update it");
      mv_diag(diags, "CCB-E3023", coord, m.buf);
      ccb_str_free(&m);
      return -1;
    }
    if (strcmp(l->sha256, e->sha256) != 0) {
      mv_diag(diags, "CCB-E3023", coord,
        "cached file hash differs from ccb.lock; run `ccb setup` to refresh it");
      return -1;
    }
  }
  for (size_t i = 0; i < locked_count; i++) {
    int found = 0;
    for (size_t j = 0; j < resolved->entries_count; j++) {
      if (strcmp(locked[i].group, resolved->entries[j].group) == 0 &&
          strcmp(locked[i].artifact, resolved->entries[j].artifact) == 0) {
        found = 1;
        break;
      }
    }
    if (!found) {
      char *coord = mv_coord_str(a, locked[i].group, locked[i].artifact,
        locked[i].version);
      mv_diag(diags, "CCB-E3023", coord,
        "is in ccb.lock but no longer resolved; run `ccb setup` to update the lockfile");
      return -1;
    }
  }
  return 0;
}

// ---------- parent POMs ----------
//
// A POM's <parent> block names a POM that is fetched from the same
// repositories (same cache and checksum rules as any other POM) and
// applied to the child: the parent's <properties>,
// <dependencyManagement>, <dependencies>, and <profiles> merge into
// the child's, with the child's own entries winning and the nearest
// ancestor winning among parents. The merged set is what ${...}
// expansion sees, so a managed version like ${lib.version} resolves
// from whichever ancestor defined the property. Parent-declared
// exclusions are not inherited (CCB has no per-dependency
// exclusions); group/version inheritance from the <parent> block
// keeps working as before. A parent version may itself be a range
// or -SNAPSHOT spec and resolves the same way a dependency does.
//
// Chain rules: at most MV_PARENT_MAX_DEPTH ancestors are followed;
// a longer chain, or a cycle (a parent coordinate repeating any
// earlier coordinate in the chain), fails with CCB-E3024 naming the
// chain. A parent POM missing from the cache in cache-only mode
// fails with CCB-E3021 like any other miss (through mv_fetch_fail).

#define MV_PARENT_MAX_DEPTH 10

// Append a parent's properties into the child's set, skipping keys
// already present: the child's own entries are in place first, and
// nearer ancestors are merged before farther ones, so first
// occurrence wins and precedence falls out of insertion order.
static void mv_merge_parent_props(ccb_arena *a, mv_pom *pom, mv_pom *parent) {
  for (size_t i = 0; i < parent->props_count; i++) {
    int seen = 0;
    for (size_t j = 0; j < pom->props_count; j++) {
      if (strcmp(pom->props[j].key, parent->props[i].key) == 0) {
        seen = 1;
        break;
      }
    }
    if (!seen) {
      MV_GROW(a, pom->props, pom->props_count, pom->props_cap, mv_prop);
      pom->props[pom->props_count++] = parent->props[i];
    }
  }
}

// Same rule for dependencyManagement, keyed on raw group:artifact.
// Even when two entries' raw keys differ but resolve equal (a
// property reference in groupId/artifactId), lookup order keeps
// precedence: the child's entries come first, then nearer ancestors.
static void mv_merge_parent_mgmt(ccb_arena *a, mv_pom *pom, mv_pom *parent) {
  for (size_t i = 0; i < parent->mgmt_count; i++) {
    mv_dep *mg = &parent->mgmt[i];
    if (mg->group == NULL || mg->artifact == NULL) {
      continue;
    }
    int seen = 0;
    for (size_t j = 0; j < pom->mgmt_count; j++) {
      if (pom->mgmt[j].group != NULL && pom->mgmt[j].artifact != NULL &&
          strcmp(pom->mgmt[j].group, mg->group) == 0 &&
          strcmp(pom->mgmt[j].artifact, mg->artifact) == 0) {
        seen = 1;
        break;
      }
    }
    if (!seen) {
      MV_GROW(a, pom->mgmt, pom->mgmt_count, pom->mgmt_cap, mv_dep);
      pom->mgmt[pom->mgmt_count++] = *mg;
    }
  }
}

// Same rule for the parent's own <dependencies>: appended after the
// child's own entries (and after nearer ancestors'), skipping a
// group:artifact the child already declares. The queue-time
// nearest-wins check is the backstop for entries whose raw keys
// differ but resolve equal.
static void mv_merge_parent_deps(ccb_arena *a, mv_pom *pom, mv_pom *parent) {
  for (size_t i = 0; i < parent->deps_count; i++) {
    mv_dep *pd = &parent->deps[i];
    if (pd->group == NULL || pd->artifact == NULL) {
      continue;
    }
    int seen = 0;
    for (size_t j = 0; j < pom->deps_count; j++) {
      if (pom->deps[j].group != NULL && pom->deps[j].artifact != NULL &&
          strcmp(pom->deps[j].group, pd->group) == 0 &&
          strcmp(pom->deps[j].artifact, pd->artifact) == 0) {
        seen = 1;
        break;
      }
    }
    if (!seen) {
      MV_GROW(a, pom->deps, pom->deps_count, pom->deps_cap, mv_dep);
      pom->deps[pom->deps_count++] = *pd;
    }
  }
}

// Same rule for the parent's <profiles>: appended after the child's
// own profiles, skipping an id the child already declares. A parent
// profile with <activeByDefault> then activates for the child too,
// which is Maven's behavior.
static void mv_merge_parent_profiles(ccb_arena *a, mv_pom *pom,
    mv_pom *parent) {
  for (size_t i = 0; i < parent->profiles_count; i++) {
    mv_profile *pp = &parent->profiles[i];
    if (pp->id == NULL) {
      continue;
    }
    int seen = 0;
    for (size_t j = 0; j < pom->profiles_count; j++) {
      if (pom->profiles[j].id != NULL &&
          strcmp(pom->profiles[j].id, pp->id) == 0) {
        seen = 1;
        break;
      }
    }
    if (!seen) {
      MV_GROW(a, pom->profiles, pom->profiles_count, pom->profiles_cap,
        mv_profile);
      pom->profiles[pom->profiles_count++] = *pp;
    }
  }
}

// Join the chain coordinates with " -> " for cycle/depth diagnostics.
static char *mv_chain_str(ccb_arena *a, char **chain, size_t count,
    const char *extra) {
  ccb_str s;
  ccb_str_init(&s);
  for (size_t i = 0; i < count; i++) {
    if (i > 0) {
      ccb_str_append(&s, " -> ");
    }
    ccb_str_append(&s, chain[i]);
  }
  if (extra != NULL) {
    ccb_str_append(&s, " -> ");
    ccb_str_append(&s, extra);
  }
  char *out = ccb_arena_strdup(a, s.buf);
  ccb_str_free(&s);
  return out;
}

// Fetch the parent chain of `pom` and merge each ancestor's
// properties, dependencyManagement, dependencies, and profiles into
// it. child_coord names the artifact being resolved (for
// diagnostics). Returns 0 on success (pom now carries the merged
// sets), -1 with a diagnostic pushed.
static int mv_merge_parents(ccb_arena *a, const char *cache, mv_repo *repos,
    size_t repos_count, mv_pom *pom, const char *child_coord, int prefetch,
    int readonly, ccb_diag_list *diags) {
  char **chain = NULL;
  size_t chain_count = 0;
  size_t chain_cap = 0;
  MV_GROW(a, chain, chain_count, chain_cap, char *);
  chain[chain_count++] = ccb_arena_strdup(a, child_coord);
  mv_pom *cur = pom;
  int depth = 0;
  while (cur->parent_group != NULL || cur->parent_artifact != NULL ||
      cur->parent_version != NULL) {
    if (cur->parent_group == NULL || cur->parent_artifact == NULL ||
        cur->parent_version == NULL) {
      mv_diag(diags, "CCB-E3019", child_coord,
        "incomplete <parent> block: groupId, artifactId, and version are all required");
      return -1;
    }
    if (depth >= MV_PARENT_MAX_DEPTH) {
      ccb_str m;
      ccb_str_init(&m);
      ccb_str_appendf(&m, "parent POM chain exceeds %d ancestors: ",
        MV_PARENT_MAX_DEPTH);
      ccb_str_append(&m, mv_chain_str(a, chain, chain_count, NULL));
      mv_diag(diags, "CCB-E3024", child_coord, m.buf);
      ccb_str_free(&m);
      return -1;
    }
    // Substitute the parent coordinates against the merged-so-far
    // set (child properties first). The project.* fallbacks use the
    // child's raw coordinates: the parent's own group/version cannot
    // be known before it is fetched.
    const char *raw_group = pom->group != NULL ? pom->group : pom->parent_group;
    const char *raw_version = pom->version != NULL ? pom->version :
      pom->parent_version;
    int bad_g = 0;
    int bad_a = 0;
    int bad_v = 0;
    char *pg = mv_subst(a, pom, raw_group, pom->artifact, raw_version,
      cur->parent_group, &bad_g);
    char *pa = mv_subst(a, pom, raw_group, pom->artifact, raw_version,
      cur->parent_artifact, &bad_a);
    char *pv = mv_subst(a, pom, raw_group, pom->artifact, raw_version,
      cur->parent_version, &bad_v);
    if (bad_g || bad_a || bad_v || pg == NULL || pa == NULL || pv == NULL ||
        !mv_part_ok(pg, strlen(pg)) || !mv_part_ok(pa, strlen(pa)) ||
        !mv_part_ok(pv, strlen(pv))) {
      mv_diag(diags, "CCB-E3019", child_coord,
        "parent coordinates cannot be resolved");
      return -1;
    }
    // A parent version may be a range or -SNAPSHOT spec: resolve it
    // to a concrete version before fetching, like any dependency.
    // The spec is kept for the directory version: a snapshot parent's
    // timestamped POM lives under its -SNAPSHOT directory.
    char *pv_spec = pv;
    char *pv_concrete = mv_resolve_version_ctx(a, cache, repos,
      repos_count, prefetch, readonly, diags, pg, pa, pv);
    if (pv_concrete == NULL) {
      return -1;
    }
    pv = pv_concrete;
    char *pcoord = mv_coord_str(a, pg, pa, pv);
    for (size_t i = 0; i < chain_count; i++) {
      if (strcmp(chain[i], pcoord) == 0) {
        ccb_str m;
        ccb_str_init(&m);
        ccb_str_append(&m, "parent POM cycle: ");
        ccb_str_append(&m, mv_chain_str(a, chain, chain_count, pcoord));
        mv_diag(diags, "CCB-E3024", child_coord, m.buf);
        ccb_str_free(&m);
        return -1;
      }
    }
    char *parent_pom_path = NULL;
    const char *pv_dir = mv_is_snapshot_spec(pv_spec) ? pv_spec : pv;
    int rc = mv_fetch(a, cache, repos, repos_count, pg, pa, pv, pv_dir,
      "pom", prefetch, readonly, &parent_pom_path);
    if (rc != 0) {
      mv_fetch_fail(diags, rc, pcoord, "parent POM");
      return -1;
    }
    char *text = NULL;
    size_t len = 0;
    if (ccb_read_file(parent_pom_path, &text, &len) != CCB_OK) {
      mv_diag(diags, "CCB-E3019", pcoord, "cached parent POM cannot be read");
      return -1;
    }
    mv_pom *pp = ccb_arena_alloc(a, sizeof(*pp));
    if (mv_parse_pom(a, text, len, pp) != 0) {
      free(text);
      mv_diag(diags, "CCB-E3019", pcoord, "malformed parent POM");
      return -1;
    }
    free(text);
    mv_merge_parent_props(a, pom, pp);
    mv_merge_parent_mgmt(a, pom, pp);
    mv_merge_parent_deps(a, pom, pp);
    mv_merge_parent_profiles(a, pom, pp);
    MV_GROW(a, chain, chain_count, chain_cap, char *);
    chain[chain_count++] = pcoord;
    depth++;
    cur = pp;
  }
  return 0;
}

// Apply the POM's active profiles: a profile is active when its id
// is listed in [deps] maven_profiles, or — when that list is empty —
// when it declares <activeByDefault>. Profiles apply after the
// parent merge, and a profile's entries win over the POM's own:
// properties and dependencyManagement replace in place, dependencies
// append after the POM's own (the queue-time nearest-wins check
// keeps the POM's own entry first). Inactive profiles contribute
// nothing: a POM declaring a dependency only inside an inactive
// profile behaves as if it had no such dependency.
static void mv_apply_profiles(ccb_arena *a, mv_pom *pom,
    const ccb_config *cfg) {
  int explicit = cfg->maven_profiles_count > 0;
  for (size_t i = 0; i < pom->profiles_count; i++) {
    mv_profile *prof = &pom->profiles[i];
    int active = 0;
    if (explicit) {
      if (prof->id != NULL) {
        for (size_t j = 0; j < cfg->maven_profiles_count; j++) {
          if (strcmp(cfg->maven_profiles[j], prof->id) == 0) {
            active = 1;
            break;
          }
        }
      }
    } else {
      active = prof->active_default;
    }
    if (!active) {
      continue;
    }
    for (size_t j = 0; j < prof->props_count; j++) {
      int seen = 0;
      for (size_t k = 0; k < pom->props_count; k++) {
        if (strcmp(pom->props[k].key, prof->props[j].key) == 0) {
          pom->props[k].val = prof->props[j].val;
          seen = 1;
          break;
        }
      }
      if (!seen) {
        MV_GROW(a, pom->props, pom->props_count, pom->props_cap, mv_prop);
        pom->props[pom->props_count++] = prof->props[j];
      }
    }
    for (size_t j = 0; j < prof->mgmt_count; j++) {
      mv_dep *mg = &prof->mgmt[j];
      if (mg->group == NULL || mg->artifact == NULL) {
        continue;
      }
      int seen = 0;
      for (size_t k = 0; k < pom->mgmt_count; k++) {
        if (pom->mgmt[k].group != NULL && pom->mgmt[k].artifact != NULL &&
            strcmp(pom->mgmt[k].group, mg->group) == 0 &&
            strcmp(pom->mgmt[k].artifact, mg->artifact) == 0) {
          pom->mgmt[k] = *mg;
          seen = 1;
          break;
        }
      }
      if (!seen) {
        MV_GROW(a, pom->mgmt, pom->mgmt_count, pom->mgmt_cap, mv_dep);
        pom->mgmt[pom->mgmt_count++] = *mg;
      }
    }
    for (size_t j = 0; j < prof->deps_count; j++) {
      MV_GROW(a, pom->deps, pom->deps_count, pom->deps_cap, mv_dep);
      pom->deps[pom->deps_count++] = prof->deps[j];
    }
  }
}

// Mutable state for one resolution run, shared by the helpers below.
typedef struct {
  ccb_arena *arena;
  const ccb_config *cfg;
  const char *cache;
  mv_repo *repos;
  size_t repos_count;
  int prefetch;
  int readonly;
  int verbose;
  mv_excl *excls;
  size_t excl_count;
  mv_qitem *queue;
  size_t qcount;
  size_t qcap;
  mv_seen *seen;
  size_t seen_count;
  size_t seen_cap;
  size_t res_aars_cap;
  size_t res_jars_cap;
  size_t res_entries_cap;
  ccb_maven_result *out;
  ccb_diag_list *diags;
} mv_resolve;

// Resolve a version spec against this run's repositories, or NULL
// with a diagnostic. Thin wrapper over mv_resolve_version_ctx.
static char *mv_resolve_version(mv_resolve *r, const char *group,
    const char *artifact, const char *spec) {
  return mv_resolve_version_ctx(r->arena, r->cache, r->repos,
    r->repos_count, r->prefetch, r->readonly, r->diags, group, artifact,
    spec);
}

// Seed the queue with the direct coordinates, in declared order.
// Returns 0 on success, -1 with a diagnostic on a malformed
// coordinate.
static int mv_seed_queue(mv_resolve *r, const ccb_config *cfg) {
  for (size_t i = 0; i < cfg->maven_count; i++) {
    char *g = NULL;
    char *art = NULL;
    char *v = NULL;
    if (!ccb_maven_parse_coord(cfg->maven[i], &g, &art, &v)) {
      mv_diag(r->diags, "CCB-E3019", cfg->maven[i], "malformed coordinate");
      free(g);
      free(art);
      free(v);
      return -1;
    }
    ccb_str ks;
    ccb_str_init(&ks);
    ccb_str_append(&ks, g);
    ccb_str_append_c(&ks, ':');
    ccb_str_append(&ks, art);
    char *key = ccb_arena_strdup(r->arena, ks.buf);
    ccb_str_free(&ks);
    // A direct version may be a range, LATEST, RELEASE, or
    // -SNAPSHOT spec: resolve it to a concrete version before the
    // seen check, so the concrete version is what wins and is
    // recorded in the lockfile. The concrete version is arena
    // memory; v stays the malloc'd parse result and is freed below.
    char *concrete = mv_resolve_version(r, g, art, v);
    if (concrete == NULL) {
      free(g);
      free(art);
      free(v);
      return -1;
    }
    mv_seen *prev = mv_seen_find(r->seen, r->seen_count, key);
    if (prev != NULL) {
      if (r->verbose && strcmp(prev->version, concrete) != 0) {
        printf("maven: %s: requested %s, using nearest %s\n", key,
          concrete, prev->version);
      }
    } else {
      MV_GROW(r->arena, r->seen, r->seen_count, r->seen_cap, mv_seen);
      r->seen[r->seen_count].key = key;
      r->seen[r->seen_count].version =
        ccb_arena_strdup(r->arena, concrete);
      r->seen_count++;
      MV_GROW(r->arena, r->queue, r->qcount, r->qcap, mv_qitem);
      r->queue[r->qcount].group = ccb_arena_strdup(r->arena, g);
      r->queue[r->qcount].artifact = ccb_arena_strdup(r->arena, art);
      r->queue[r->qcount].version = ccb_arena_strdup(r->arena, concrete);
      r->queue[r->qcount].dir_version = mv_is_snapshot_spec(v) ?
        ccb_arena_strdup(r->arena, v) :
        ccb_arena_strdup(r->arena, concrete);
      r->queue[r->qcount].depth = 0;
      r->qcount++;
    }
    free(g);
    free(art);
    free(v);
  }
  return 0;
}

// Resolve a dependency's version from this POM's dependencyManagement
// entry for the same group:artifact. Returns the substituted version,
// or NULL when there is no matching entry (or its version fails to
// substitute, with *dbad set). The first matching entry wins.
static char *mv_resolve_version_from_mgmt(ccb_arena *a, mv_pom *pom,
    const char *eff_group, const char *eff_artifact, const char *eff_version,
    const char *dg, const char *da, int *dbad) {
  *dbad = 0;
  for (size_t m = 0; m < pom->mgmt_count; m++) {
    mv_dep *mg = &pom->mgmt[m];
    int mbad = 0;
    char *mg_g = mv_subst(a, pom, eff_group, eff_artifact, eff_version,
      mg->group, &mbad);
    char *mg_a = mv_subst(a, pom, eff_group, eff_artifact, eff_version,
      mg->artifact, &mbad);
    if (mbad || mg_g == NULL || mg_a == NULL) {
      continue;
    }
    if (strcmp(mg_g, dg) == 0 && strcmp(mg_a, da) == 0 &&
        mg->version != NULL) {
      char *dv = mv_subst(a, pom, eff_group, eff_artifact, eff_version,
        mg->version, dbad);
      if (*dbad) {
        return NULL;
      }
      return dv;
    }
  }
  return NULL;
}

// Resolve a POM's effective coordinates: its own values, else the
// embedded parent block. Returns 0 on success, -1 with a diagnostic.
static int mv_effective_coords(ccb_arena *a, mv_pom *pom, const char *coord,
    ccb_diag_list *diags, char **eff_group, char **eff_artifact,
    char **eff_version) {
  int bad = 0;
  const char *raw_group = pom->group != NULL ? pom->group : pom->parent_group;
  const char *raw_version = pom->version != NULL ? pom->version : pom->parent_version;
  *eff_group = mv_subst(a, pom, raw_group, pom->artifact, raw_version,
    raw_group, &bad);
  *eff_artifact = mv_subst(a, pom, raw_group, pom->artifact, raw_version,
    pom->artifact, &bad);
  *eff_version = mv_subst(a, pom, raw_group, pom->artifact, raw_version,
    raw_version, &bad);
  if (bad || *eff_group == NULL || *eff_artifact == NULL ||
      *eff_version == NULL) {
    mv_diag(diags, "CCB-E3019", coord,
      "POM coordinates cannot be resolved (missing value or unresolvable property)");
    return -1;
  }
  return 0;
}

// Resolve a POM's packaging to the artifact extension to fetch:
// "aar" or "jar", or NULL for pom packaging (no artifact file).
// Returns 0 on success, -1 with a diagnostic.
static int mv_packaging_ext(ccb_arena *a, mv_pom *pom, const char *eff_group,
    const char *eff_artifact, const char *eff_version, const char *coord,
    ccb_diag_list *diags, const char **ext_out, int *is_aar_out) {
  const char *packaging = "jar";
  if (pom->packaging != NULL) {
    int pbad = 0;
    char *p = mv_subst(a, pom, eff_group, eff_artifact, eff_version,
      pom->packaging, &pbad);
    if (pbad || p == NULL) {
      mv_diag(diags, "CCB-E3019", coord, "packaging cannot be resolved");
      return -1;
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
    return -1;
  }
  *ext_out = ext;
  *is_aar_out = is_aar;
  return 0;
}

// Resolve one transitive dependency of a POM: skip test/provided/
// system/import scopes and optional dependencies, fall back to
// dependencyManagement for a missing version, skip excluded and
// already-seen artifacts (nearest wins), else enqueue it. Returns 0
// when the dependency is queued or skipped, -1 with a diagnostic on
// failure.
static int mv_queue_dep(mv_resolve *r, mv_pom *pom, const char *eff_group,
    const char *eff_artifact, const char *eff_version, const char *coord,
    mv_dep *dep, int item_depth) {
  int dbad = 0;
  char *scope = dep->scope != NULL ?
    mv_subst(r->arena, pom, eff_group, eff_artifact, eff_version,
      dep->scope, &dbad) : (char *)"compile";
  if (dbad) {
    mv_diag(r->diags, "CCB-E3019", coord,
      "dependency scope cannot be resolved");
    return -1;
  }
  if (strcmp(scope, "test") == 0 || strcmp(scope, "provided") == 0 ||
      strcmp(scope, "system") == 0 || strcmp(scope, "import") == 0) {
    return 0;
  }
  char *optional = dep->optional != NULL ?
    mv_subst(r->arena, pom, eff_group, eff_artifact, eff_version,
      dep->optional, &dbad) : NULL;
  if (dbad) {
    mv_diag(r->diags, "CCB-E3019", coord,
      "dependency optional flag cannot be resolved");
    return -1;
  }
  if (optional != NULL && strcmp(optional, "true") == 0) {
    return 0;
  }
  char *dg = mv_subst(r->arena, pom, eff_group, eff_artifact, eff_version,
    dep->group, &dbad);
  char *da = mv_subst(r->arena, pom, eff_group, eff_artifact, eff_version,
    dep->artifact, &dbad);
  char *dv = dep->version != NULL ?
    mv_subst(r->arena, pom, eff_group, eff_artifact, eff_version,
      dep->version, &dbad) : NULL;
  if (dbad || dg == NULL || da == NULL ||
      !mv_part_ok(dg, strlen(dg)) || !mv_part_ok(da, strlen(da))) {
    mv_diag(r->diags, "CCB-E3019", coord,
      "dependency coordinates cannot be resolved");
    return -1;
  }
  if (dv != NULL && !mv_part_ok(dv, strlen(dv))) {
    mv_diag(r->diags, "CCB-E3019", coord,
      "dependency version is path-unsafe");
    return -1;
  }
  if (dv == NULL || dv[0] == '\0') {
    // Fall back to this POM's dependencyManagement entry.
    dv = mv_resolve_version_from_mgmt(r->arena, pom, eff_group, eff_artifact,
      eff_version, dg, da, &dbad);
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
    mv_diag(r->diags, "CCB-E3019", coord, m.buf);
    ccb_str_free(&m);
    return -1;
  }
  // A transitive version may also be a range, LATEST, RELEASE, or
  // -SNAPSHOT spec: resolve it to a concrete version here, so the
  // exclusion check and nearest-wins both see the version that will
  // actually be fetched. The spec is kept for the directory version:
  // a snapshot's timestamped files live under its -SNAPSHOT
  // directory.
  char *spec = dv;
  dv = mv_resolve_version(r, dg, da, dv);
  if (dv == NULL) {
    return -1;
  }
  // Exclusions are checked before nearest-wins: an excluded
  // artifact never enters the seen set, so an excluded version
  // can never win and another version from a later path can.
  // The subtree is pruned with it: an excluded artifact's own
  // dependencies are never queued.
  if (mv_is_excluded(r->excls, r->excl_count, dg, da, dv)) {
    if (r->verbose) {
      printf("maven: %s:%s:%s: excluded, skipping\n", dg, da, dv);
    }
    return 0;
  }
  ccb_str ks;
  ccb_str_init(&ks);
  ccb_str_append(&ks, dg);
  ccb_str_append_c(&ks, ':');
  ccb_str_append(&ks, da);
  char *key = ccb_arena_strdup(r->arena, ks.buf);
  ccb_str_free(&ks);
  mv_seen *prev = mv_seen_find(r->seen, r->seen_count, key);
  if (prev != NULL) {
    if (r->verbose && strcmp(prev->version, dv) != 0) {
      printf("maven: %s: requested %s, using nearest %s\n", key, dv,
        prev->version);
    }
    return 0;
  }
  MV_GROW(r->arena, r->seen, r->seen_count, r->seen_cap, mv_seen);
  r->seen[r->seen_count].key = key;
  r->seen[r->seen_count].version = ccb_arena_strdup(r->arena, dv);
  r->seen_count++;
  MV_GROW(r->arena, r->queue, r->qcount, r->qcap, mv_qitem);
  r->queue[r->qcount].group = dg;
  r->queue[r->qcount].artifact = da;
  r->queue[r->qcount].version = dv;
  r->queue[r->qcount].dir_version = mv_is_snapshot_spec(spec) ? spec : dv;
  r->queue[r->qcount].depth = item_depth + 1;
  r->qcount++;
  return 0;
}

// Resolve one queued coordinate: fetch and parse its POM, merge its
// parent chain, apply its active profiles, fetch its artifact,
// record it, and queue its transitive dependencies. Returns 0 on
// success, -1 with a diagnostic on failure.
//
// walk == 0 is the compiler-plugin path: no transitive walk and no
// lockfile entry, and the artifact must be a jar, whose cache path
// is returned through *jar_out (arena memory; NULL when ext is NULL
// is a failure, not a pom entry — a plugin has to be a jar).
static int mv_resolve_one(mv_resolve *r, mv_qitem item, int walk,
    char **jar_out) {
  const char *coord = mv_coord_str(r->arena, item.group, item.artifact,
    item.version);
  char *pom_path = NULL;
  int rc = mv_fetch(r->arena, r->cache, r->repos, r->repos_count, item.group,
    item.artifact, item.version, item.dir_version, "pom", r->prefetch,
    r->readonly, &pom_path);
  if (rc != 0) {
    mv_fetch_fail(r->diags, rc, coord, "POM");
    return -1;
  }
  char *pom_text = NULL;
  size_t pom_len = 0;
  if (ccb_read_file(pom_path, &pom_text, &pom_len) != CCB_OK) {
    mv_diag(r->diags, "CCB-E3019", coord, "cached POM cannot be read");
    return -1;
  }
  mv_pom pom;
  if (mv_parse_pom(r->arena, pom_text, pom_len, &pom) != 0) {
    free(pom_text);
    mv_diag(r->diags, "CCB-E3019", coord, "malformed POM");
    return -1;
  }
  free(pom_text);
  // Fetch the parent chain and merge its properties,
  // dependencyManagement, dependencies, and profiles into this POM
  // (child entries win), then apply this POM's own active profiles.
  if (mv_merge_parents(r->arena, r->cache, r->repos, r->repos_count, &pom,
      coord, r->prefetch, r->readonly, r->diags) != 0) {
    return -1;
  }
  mv_apply_profiles(r->arena, &pom, r->cfg);
  char *eff_group = NULL;
  char *eff_artifact = NULL;
  char *eff_version = NULL;
  if (mv_effective_coords(r->arena, &pom, coord, r->diags, &eff_group,
      &eff_artifact, &eff_version) != 0) {
    return -1;
  }
  const char *ext = NULL;
  int is_aar = 0;
  if (mv_packaging_ext(r->arena, &pom, eff_group, eff_artifact, eff_version,
      coord, r->diags, &ext, &is_aar) != 0) {
    return -1;
  }
  if (ext != NULL) {
    char *art_path = NULL;
    rc = mv_fetch(r->arena, r->cache, r->repos, r->repos_count, item.group,
      item.artifact, item.version, item.dir_version, ext, r->prefetch,
      r->readonly, &art_path);
    if (rc != 0) {
      mv_fetch_fail(r->diags, rc, coord, ext);
      return -1;
    }
    if (!walk) {
      if (is_aar) {
        mv_diag(r->diags, "CCB-E3019", coord,
          "a compiler plugin must be a jar artifact, not an aar");
        return -1;
      }
      *jar_out = art_path;
      return 0;
    }
    if (is_aar) {
      mv_push_result(&r->out->aars, &r->out->aars_count, &r->res_aars_cap,
        art_path);
    } else {
      mv_push_result(&r->out->jars, &r->out->jars_count, &r->res_jars_cap,
        art_path);
    }
    // Lockfile entry: hash of the artifact file.
    char hex[65];
    if (ccb_sha256_file(art_path, hex) != CCB_OK) {
      mv_diag(r->diags, "CCB-E3019", coord, "resolved file cannot be read");
      return -1;
    }
    mv_push_entry(r->out, &r->res_entries_cap, item.group, item.artifact,
      item.version, ext, hex, art_path);
  } else {
    if (!walk) {
      mv_diag(r->diags, "CCB-E3019", coord,
        "a compiler plugin must be a jar artifact, not a pom");
      return -1;
    }
    // Pom packaging has no artifact file: the lockfile records the
    // POM's hash, so a changed BOM still fails enforcement.
    char hex[65];
    if (ccb_sha256_file(pom_path, hex) != CCB_OK) {
      mv_diag(r->diags, "CCB-E3019", coord, "resolved POM cannot be read");
      return -1;
    }
    mv_push_entry(r->out, &r->res_entries_cap, item.group, item.artifact,
      item.version, "pom", hex, pom_path);
  }
  // Transitive dependencies of this POM.
  for (size_t d = 0; d < pom.deps_count; d++) {
    if (mv_queue_dep(r, &pom, eff_group, eff_artifact, eff_version, coord,
        &pom.deps[d], item.depth) != 0) {
      return -1;
    }
  }
  return 0;
}

// ---------- JVM-platform variants ----------

// A `-jvm` artifact whose `-android` sibling (same group, same
// version) is also resolved is the JVM-platform variant of a Kotlin
// multiplatform library: the Android build only wants the Android
// variant, and dex rejects the classes both define. A `-jvm`
// artifact with no `-android` sibling is kept: pure-JVM libraries
// such as kotlinx-coroutines-core-jvm have no Android variant.
static int mv_is_android_sibling(const ccb_maven_entry *jvm,
    const ccb_maven_entry *other) {
  size_t base_len = strlen(jvm->artifact) - 4; // strip "-jvm"
  if (strcmp(jvm->group, other->group) != 0) {
    return 0;
  }
  if (strcmp(jvm->version, other->version) != 0) {
    return 0;
  }
  if (strncmp(other->artifact, jvm->artifact, base_len) != 0) {
    return 0;
  }
  return strcmp(other->artifact + base_len, "-android") == 0;
}

static void mv_drop_path(char **arr, size_t *count, const char *path) {
  size_t w = 0;
  for (size_t i = 0; i < *count; i++) {
    if (strcmp(arr[i], path) == 0) {
      free(arr[i]);
      continue;
    }
    if (w != i) {
      arr[w] = arr[i];
    }
    w++;
  }
  *count = w;
}

static void mv_drop_jvm_variants(ccb_maven_result *out) {
  size_t w = 0;
  for (size_t i = 0; i < out->entries_count; i++) {
    ccb_maven_entry *e = &out->entries[i];
    size_t len = strlen(e->artifact);
    int drop = 0;
    if (len > 4 && strcmp(e->artifact + len - 4, "-jvm") == 0) {
      for (size_t j = 0; j < out->entries_count; j++) {
        if (j != i && mv_is_android_sibling(e, &out->entries[j])) {
          drop = 1;
          break;
        }
      }
    }
    if (drop) {
      // The dropped entry's path is also a dex/classpath input in
      // the jars/aars arrays: drop it there too, or the duplicate
      // class still reaches d8.
      mv_drop_path(out->jars, &out->jars_count, e->path);
      mv_drop_path(out->aars, &out->aars_count, e->path);
      free(e->group);
      free(e->artifact);
      free(e->version);
      free(e->packaging);
      free(e->sha256);
      free(e->path);
      continue;
    }
    if (w != i) {
      out->entries[w] = out->entries[i];
    }
    w++;
  }
  out->entries_count = w;
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
  mv_repo *repos = mv_repos(arena, cfg, &repos_count, diags);
  if (repos == NULL) {
    return CCB_ERR_TOOLCHAIN;
  }
  size_t excl_count = 0;
  mv_excl *excls = mv_exclusions(arena, cfg, &excl_count);

  mv_resolve r;
  memset(&r, 0, sizeof(r));
  r.arena = arena;
  r.cfg = cfg;
  r.cache = cache;
  r.repos = repos;
  r.repos_count = repos_count;
  r.prefetch = prefetch;
  r.readonly = readonly;
  r.verbose = verbose;
  r.excls = excls;
  r.excl_count = excl_count;
  r.out = out;
  r.diags = diags;

  // Seed the queue with the direct coordinates, in declared order.
  if (mv_seed_queue(&r, cfg) != 0) {
    goto fail;
  }

  // Breadth-first walk: the first visit to a group:artifact is the
  // nearest, so it wins and later (deeper or later-declared) versions
  // are skipped.
  size_t head = 0;
  while (head < r.qcount) {
    mv_qitem item = r.queue[head++];
    if (mv_resolve_one(&r, item, 1, NULL) != 0) {
      goto fail;
    }
  }
  // JVM-platform variants drop out when the Android variant of the
  // same library is present (see mv_drop_jvm_variants).
  mv_drop_jvm_variants(out);
  // Lockfile enforcement (cache-only mode only): `ccb setup` writes
  // the lockfile; check and build refuse any deviation from it.
  // A missing lockfile means no enforcement: pre-lockfile behavior.
  if (!prefetch && cfg->project_root != NULL) {
    char *lock_path = ccb_path_join(cfg->project_root, "ccb.lock");
    if (ccb_path_exists(lock_path)) {
      if (mv_lock_enforce(arena, lock_path, out, diags) != 0) {
        free(lock_path);
        goto fail;
      }
    }
    free(lock_path);
  }
  return CCB_OK;

fail:
  ccb_maven_result_free(out);
  memset(out, 0, sizeof(*out));
  return CCB_ERR_TOOLCHAIN;
}

ccb_status ccb_maven_resolve_plugins(const ccb_config *cfg, int prefetch,
    int verbose, int readonly, char ***jars_out, size_t *count_out,
    ccb_diag_list *diags) {
  *jars_out = NULL;
  *count_out = 0;
  if (cfg->kotlin_plugins_count == 0) {
    return CCB_OK;
  }
  (void)verbose;
  CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
  CCB_AUTO_FREE char *cache = ccb_maven_cache_dir();
  if (cache == NULL) {
    mv_diag(diags, "CCB-E3019", NULL,
      "no Maven cache directory available (set HOME or XDG_CACHE_HOME)");
    return CCB_ERR_TOOLCHAIN;
  }
  size_t repos_count = 0;
  mv_repo *repos = mv_repos(arena, cfg, &repos_count, diags);
  if (repos == NULL) {
    return CCB_ERR_TOOLCHAIN;
  }
  mv_resolve r;
  memset(&r, 0, sizeof(r));
  r.arena = arena;
  r.cfg = cfg;
  r.cache = cache;
  r.repos = repos;
  r.repos_count = repos_count;
  r.prefetch = prefetch;
  r.readonly = readonly;
  r.diags = diags;

  char **jars = NULL;
  size_t njars = 0;
  size_t cap = 0;
  for (size_t i = 0; i < cfg->kotlin_plugins_count; i++) {
    char *g = NULL;
    char *art = NULL;
    char *v = NULL;
    if (!ccb_maven_parse_coord(cfg->kotlin_plugins[i], &g, &art, &v)) {
      mv_diag(diags, "CCB-E3019", cfg->kotlin_plugins[i],
        "malformed plugin coordinate");
      free(g);
      free(art);
      free(v);
      goto fail;
    }
    mv_qitem item;
    item.group = g;
    item.artifact = art;
    item.version = v;
    // Plugins are exact versions (enforced at config load), so the
    // directory version is the version itself.
    item.dir_version = v;
    item.depth = 0;
    char *jar = NULL;
    if (mv_resolve_one(&r, item, 0, &jar) != 0) {
      free(g);
      free(art);
      free(v);
      goto fail;
    }
    free(g);
    free(art);
    free(v);
    if (njars == cap) {
      size_t ncap = cap == 0 ? 4 : cap * 2;
      jars = ccb_xrealloc(jars, ncap * sizeof(*jars));
      cap = ncap;
    }
    jars[njars++] = ccb_xstrdup(jar);
  }
  *jars_out = jars;
  *count_out = njars;
  return CCB_OK;

fail:
  ccb_free_str_array(jars, njars);
  *jars_out = NULL;
  *count_out = 0;
  return CCB_ERR_TOOLCHAIN;
}

// ---------- lockfile writing ----------

static int mv_entry_cmp(const void *pa, const void *pb) {
  const ccb_maven_entry *a = *(const ccb_maven_entry *const *)pa;
  const ccb_maven_entry *b = *(const ccb_maven_entry *const *)pb;
  int c = strcmp(a->group, b->group);
  if (c != 0) {
    return c;
  }
  return strcmp(a->artifact, b->artifact);
}

ccb_status ccb_maven_write_lockfile(const ccb_config *cfg,
    const ccb_maven_result *r, ccb_diag_list *diags) {
  if (cfg->project_root == NULL) {
    mv_diag(diags, "CCB-E3019", "ccb.lock",
      "no project root, cannot write the lockfile");
    return CCB_ERR_TOOLCHAIN;
  }
  // Sort entry pointers for stable, diff-friendly output: the file
  // changes only when the resolved set changes, never when the
  // declaration order does.
  const ccb_maven_entry **sorted = NULL;
  if (r->entries_count > 0) {
    sorted = ccb_xmalloc(r->entries_count * sizeof(*sorted));
    for (size_t i = 0; i < r->entries_count; i++) {
      sorted[i] = &r->entries[i];
    }
    qsort(sorted, r->entries_count, sizeof(*sorted), mv_entry_cmp);
  }
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s,
    "# ccb.lock: resolved Maven tree, generated by `ccb setup`.\n"
    "# Commit this file. `ccb check` and `ccb build` enforce it: any\n"
    "# deviation from these versions or hashes fails with CCB-E3023.\n"
    "# ccb-lock-version: 1\n");
  for (size_t i = 0; i < r->entries_count; i++) {
    const ccb_maven_entry *e = sorted[i];
    ccb_str_append(&s, e->group);
    ccb_str_append_c(&s, ':');
    ccb_str_append(&s, e->artifact);
    ccb_str_append_c(&s, ':');
    ccb_str_append(&s, e->version);
    ccb_str_append_c(&s, ' ');
    ccb_str_append(&s, e->packaging);
    ccb_str_append_c(&s, ' ');
    ccb_str_append(&s, e->sha256);
    ccb_str_append_c(&s, '\n');
  }
  free(sorted);
  char *lock_path = ccb_path_join(cfg->project_root, "ccb.lock");
  ccb_str t;
  ccb_str_init(&t);
  ccb_str_append(&t, lock_path);
  ccb_str_append(&t, ".tmp");
  char *tmp = ccb_str_steal(&t);
  ccb_status rc = CCB_OK;
  if (ccb_write_file(tmp, s.buf, strlen(s.buf)) != CCB_OK ||
      rename(tmp, lock_path) != 0) {
    remove(tmp);
    mv_diag(diags, "CCB-E3019", "ccb.lock",
      "lockfile could not be written");
    rc = CCB_ERR_TOOLCHAIN;
  }
  free(tmp);
  free(lock_path);
  ccb_str_free(&s);
  return rc;
}
