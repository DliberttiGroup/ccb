// ccb_cache: the incremental-build manifest (build/ccb-cache.json).
// A missing or corrupt cache is not an error: it just means "rebuild
// everything". Granularity is per pipeline STEP (see docs): when a
// step's inputs are unchanged its outputs are reused wholesale; when
// any input changes the whole step re-runs. Tool upgrades change the
// tool fingerprint and invalidate everything.
#ifndef CCB_CACHE_H
#define CCB_CACHE_H

#include "ccb_common.h"

typedef struct {
  char *tool_fingerprint;
  char **step_names;
  char **step_fps;
  size_t step_count;
  size_t step_cap;
  char **file_paths;
  char **file_hashes;
  size_t file_count;
  size_t file_cap;
} ccb_cache;

// Missing or unreadable files yield an empty cache with CCB_OK.
ccb_status ccb_cache_load(const char *path, ccb_cache **out);
// Saves atomically (temp file + rename): an interrupted save never
// leaves a torn cache behind. Entries are written sorted by key, so
// the file is byte-identical for equivalent caches regardless of the
// order steps were recorded in (see docs/reproducibility.md).
ccb_status ccb_cache_save(const ccb_cache *cache, const char *path);
void ccb_cache_free(ccb_cache *cache);
const char *ccb_cache_step_fp(const ccb_cache *c, const char *step);
void ccb_cache_set_step(ccb_cache *c, const char *step, const char *fp);
const char *ccb_cache_file_hash(const ccb_cache *c, const char *relpath);
void ccb_cache_set_file(ccb_cache *c, const char *relpath, const char *hash);
// SHA-256 hex of a file's contents; NULL on read failure.
char *ccb_cache_hash_file(const char *path);
// Fingerprint of key/value pairs: sha256 over "key=value\n" lines.
char *ccb_cache_fingerprint(const char **keys, const char **values, size_t count);

// Per-task cache delta (Step 8): the step fingerprints one task
// completed, owned by that task until it is merged. A task never
// writes the shared cache itself: fingerprinting/planning runs
// single-threaded before any worker starts, and the driver merges
// each completed task's delta into the cache (ccb_cache_delta_merge)
// from the single publishing thread, after the task's outputs
// exist. A failed task's delta is discarded with the task, so a
// failed step's fingerprint is never published; the cache file
// itself is published once per build, atomically, by
// ccb_cache_save (temp file + rename) from that same thread.
typedef struct {
  char **step_names;
  char **step_fps;
  size_t step_count;
  size_t step_cap;
} ccb_cache_delta;

void ccb_cache_delta_init(ccb_cache_delta *d);
void ccb_cache_delta_free(ccb_cache_delta *d);
void ccb_cache_delta_set_step(ccb_cache_delta *d, const char *step,
  const char *fp);
void ccb_cache_delta_merge(ccb_cache *c, const ccb_cache_delta *d);

#endif
