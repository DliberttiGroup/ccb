// ccb_maven_internal.h: shared internals for the ccb_maven.c split:
// file-local types, macros, and the helpers called across the split
// files. Not public API: only the split files include this header.
#ifndef CCB_MAVEN_INTERNAL_H
#define CCB_MAVEN_INTERNAL_H

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_str.h"
#include <ctype.h>

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

#define MV_GOOGLE_MAVEN "https://dl.google.com/dl/android/maven2/"

#define MV_MAVEN_CENTRAL "https://repo.maven.apache.org/maven2/"

typedef struct {
  char *base;
  char *user; // NULL: no auth
  char *pass; // NULL when user is NULL
} mv_repo;

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

typedef struct {
  int is_num; // 1: digit run, 0: qualifier string
  const char *text;
  size_t len;
} mv_vtok;

typedef struct {
  char *low; // NULL: unbounded below
  int low_inc;
  char *high; // NULL: unbounded above
  int high_inc;
} mv_range;

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

typedef struct {
  char *group;
  char *artifact;
  char *version; // NULL: every version of the artifact is excluded
} mv_excl;

typedef struct {
  char *group;
  char *artifact;
  char *version;
  char *packaging;
  char *sha256;
} mv_lock_entry;

#define MV_PARENT_MAX_DEPTH 10

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

void mv_diag(ccb_diag_list *diags, const char *code, const char *coord,
    const char *msg);
int mv_part_ok(const char *s, size_t n);
int ccb_maven_parse_coord(const char *s, char **group, char **artifact, char **version);
char *mv_group_path(ccb_arena *a, const char *group);
char *mv_coord_str(ccb_arena *a, const char *group, const char *artifact,
    const char *version);
mv_repo *mv_repos(ccb_arena *a, const ccb_config *cfg,
    size_t *count, ccb_diag_list *diags);
char *ccb_maven_cache_dir(void);
char *mv_cache_file(ccb_arena *a, const char *cache, const char *group,
    const char *artifact, const char *version, const char *leaf);
char *mv_leaf(ccb_arena *a, const char *artifact, const char *version,
    const char *ext);
char *mv_url(ccb_arena *a, const char *base, const char *group,
    const char *artifact, const char *version, const char *leaf);
int mv_verify_downloaded(ccb_arena *a, const char *tmp_path,
    const char *url, const char *user, const char *pass,
    char *sha256_hex);
void mv_write_sidecar(const char *final_path, const char *sha256_hex);
int mv_cache_file_ok(const char *final_path, int readonly);
char *mv_trim(char *s);
int mv_parse_pom(ccb_arena *a, const char *text, size_t len, mv_pom *pom);
char *mv_subst(ccb_arena *a, mv_pom *pom, const char *eff_group,
    const char *eff_artifact, const char *eff_version, const char *value,
    int *bad);
int mv_fetch(ccb_arena *a, const char *cache, mv_repo *repos,
    size_t repos_count, const char *group, const char *artifact,
    const char *version, const char *dir_version, const char *ext,
    int prefetch, int readonly, char **final_out);
int mv_version_cmp(const char *a, const char *b);
int mv_is_snapshot_spec(const char *s);
char *mv_resolve_version_ctx(ccb_arena *a, const char *cache,
    mv_repo *repos, size_t repos_count, int prefetch, int readonly,
    ccb_diag_list *diags, const char *group, const char *artifact,
    const char *spec);
ccb_status mv_fetch_fail(ccb_diag_list *diags, int rc,
    const char *coord, const char *what);
void ccb_maven_result_free(ccb_maven_result *r);
mv_excl *mv_exclusions(ccb_arena *a, const ccb_config *cfg,
    size_t *count);
int mv_lock_enforce(ccb_arena *a, const char *lock_path,
    const ccb_maven_result *resolved, ccb_diag_list *diags);
int mv_seed_queue(mv_resolve *r, const ccb_config *cfg);
int mv_resolve_one(mv_resolve *r, mv_qitem item, int walk,
    char **jar_out);

#endif // CCB_MAVEN_INTERNAL_H
