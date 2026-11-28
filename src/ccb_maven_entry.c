// ccb_maven_entry: public resolve entry points, plugin resolution,
// JVM-platform variant filtering, and lockfile writing.
// Split from ccb_maven.c.

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_str.h"
#include <ctype.h>
#include "ccb_maven_internal.h"

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

static void mv_note_unmatched_profiles(mv_resolve *r,
    const ccb_config *cfg) {
  if (!r->verbose) {
    return;
  }
  for (size_t i = 0; i < cfg->maven_profiles_count; i++) {
    int matched = 0;
    for (size_t j = 0; j < r->matched_count; j++) {
      if (strcmp(cfg->maven_profiles[i], r->matched_profiles[j]) == 0) {
        matched = 1;
        break;
      }
    }
    if (!matched) {
      printf("maven: profile '%s' in [deps] maven_profiles matched no resolved POM\n",
        cfg->maven_profiles[i]);
    }
  }
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
  // A listed profile id that matched no resolved POM is usually a
  // typo: say so verbosely instead of silently activating nothing.
  mv_note_unmatched_profiles(&r, cfg);
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
