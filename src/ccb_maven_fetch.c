// ccb_maven_fetch: artifact/metadata fetching + Maven version ranges.
// Split from ccb_maven.c: downloads, maven-metadata.xml, and the
// range/snapshot version resolution context live here.

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_str.h"
#include <ctype.h>
#include "ccb_maven_internal.h"

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
int mv_fetch(ccb_arena *a, const char *cache, mv_repo *repos,
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
// cache-only never touches the network. When every repository fails
// to serve fresh metadata during prefetch, the cached copy is
// reused when it exists and its recorded checksum still verifies,
// so setup still works offline for ranges and snapshots it already
// resolved; a cached copy that fails its checksum reports 3.
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
  // Every repository failed to serve fresh metadata: fall back to
  // the cached copy when one exists and its recorded checksum still
  // verifies. A cached copy that fails its checksum reports
  // corrupt (3) instead of missing (1).
  if (ccb_path_exists(final_path)) {
    if (mv_cache_file_ok(final_path, readonly)) {
      *path_out = final_path;
      return 0;
    }
    return 3;
  }
  return 1;
}

// ---------- version ranges ----------

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

int mv_is_snapshot_spec(const char *s) {
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
char *mv_resolve_version_ctx(ccb_arena *a, const char *cache,
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
