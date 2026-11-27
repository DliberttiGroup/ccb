// ccb_maven_resolve: the dependency resolver core (dense logic, kept whole).
// Split from ccb_maven.c: queue, exclusions, lockfile, parent POMs, and the
// per-artifact resolve walk. This is one recursive resolution algorithm;
// splitting it further would scatter it across files for no gain.

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_str.h"
#include <ctype.h>
#include "ccb_maven_internal.h"

// ---------- resolution ----------

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
ccb_status mv_fetch_fail(ccb_diag_list *diags, int rc,
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

// Parse cfg->maven_exclude into arena entries. Malformed entries are
// skipped: config load validates the list, so resolve never sees one
// through a real config; a hand-built config just loses that entry.
mv_excl *mv_exclusions(ccb_arena *a, const ccb_config *cfg,
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
int mv_lock_enforce(ccb_arena *a, const char *lock_path,
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
int mv_seed_queue(mv_resolve *r, const ccb_config *cfg) {
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
int mv_resolve_one(mv_resolve *r, mv_qitem item, int walk,
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
