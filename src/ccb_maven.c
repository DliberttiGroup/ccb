#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_str.h"
#include <ctype.h>
#include "ccb_maven_internal.h"

// ccb_maven: remote Maven dependency resolution (see ccb_maven.h).

// Growable arena arrays: appends copy into a fresh arena block when
// full. Counts here are small (dependencies per project), so the
// copy cost is negligible and no array ever needs individual freeing.

void mv_diag(ccb_diag_list *diags, const char *code, const char *coord,
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

int mv_part_ok(const char *s, size_t n) {
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

char *mv_group_path(ccb_arena *a, const char *group) {
  char *p = ccb_arena_strdup(a, group);
  for (char *q = p; *q != '\0'; q++) {
    if (*q == '.') {
      *q = '/';
    }
  }
  return p;
}

char *mv_coord_str(ccb_arena *a, const char *group, const char *artifact,
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

// One repository: a normalized base URL plus optional Basic-auth
// credentials. The password lives only in arena memory and travels
// only in a curl argv; it never appears in a URL, a log line, or a
// diagnostic.

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
mv_repo *mv_repos(ccb_arena *a, const ccb_config *cfg,
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
char *mv_cache_file(ccb_arena *a, const char *cache, const char *group,
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

char *mv_leaf(ccb_arena *a, const char *artifact, const char *version,
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

char *mv_url(ccb_arena *a, const char *base, const char *group,
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
int mv_verify_downloaded(ccb_arena *a, const char *tmp_path,
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
void mv_write_sidecar(const char *final_path, const char *sha256_hex) {
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
int mv_cache_file_ok(const char *final_path, int readonly) {
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

// ---------- Maven version ordering ----------
//
// ComparableVersion rules, simplified to the cases real repositories
// publish: dot/dash-separated tokens, digit runs compared numerically
// (arbitrary precision via length-then-lexical), qualifier order
// alpha < beta < milestone < rc < snapshot < "" (release) < sp, and
// unknown qualifiers sorting after sp, lexically. "1.0" == "1.0.0";
// "1.0-alpha" < "1.0"; "1.0-SNAPSHOT" < "1.0" < "1.0-sp-1".

// Read one token starting at *pp (past '.'/'-' separators). Returns
// 0 when no token remains. *tok is written on every path, including
// the no-token path (an empty qualifier at the end of the string):
// the caller reads the token only when 1 is returned, but leaving an
// out-parameter untouched on one return path is exactly what made
// -O3 -Werror=maybe-uninitialized fail in mv_version_cmp, so the
// contract here is that *tok is always defined.
static int mv_next_vtok(const char **pp, mv_vtok *tok) {
  const char *p = *pp;
  while (*p == '.' || *p == '-') {
    p++;
  }
  tok->is_num = 0;
  tok->text = p;
  tok->len = 0;
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

int mv_version_cmp(const char *a, const char *b) {
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
