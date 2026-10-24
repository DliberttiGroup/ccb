// ccb_maven: remote Maven dependency resolution (spec §5.4).
//
// A declared coordinate `group:artifact:version` resolves through the
// configured Maven repositories into a local AAR or JAR that feeds the
// existing deps step exactly like a local file. Packaging comes from
// the artifact's POM: `aar` wins as an AAR, `jar` as a JAR.
//
// Hermeticity: this module never downloads during a build. It has two
// modes: prefetch (used by `ccb setup`, the only network command)
// downloads POMs and artifacts into the Maven cache, and cache-only
// (used by builds and `ccb check`) reads the cache and fails with
// CCB-E3021 when an artifact was never prefetched. Run `ccb setup`
// after adding a coordinate; builds then stay offline.
//
// Rules (v1): exact versions only (no ranges, LATEST, RELEASE, or
// -SNAPSHOT — rejected at config load); Google Maven by default,
// Maven Central only when `[deps] maven_central = true`; transitive
// resolution over compile + runtime scopes (test/provided/optional
// skipped), nearest-wins on version conflict, cycle-safe. Checksums:
// a published .sha256 (else .sha1) is verified on download and a
// mismatch fails with CCB-E3020; when a repo publishes no checksum,
// the computed SHA-256 is recorded in a `.ccb-sha256` sidecar next to
// the cached file and verified on later cache hits.
#ifndef CCB_MAVEN_H
#define CCB_MAVEN_H

#include "ccb_common.h"
#include "ccb_config.h"
#include "ccb_diag.h"

// Resolved artifacts. All paths are malloc'd cache paths owned by the
// struct; free with ccb_maven_result_free. Order is deterministic:
// direct dependencies in declared order, then transitive dependencies
// level by level (BFS).
typedef struct {
  char **aars;
  size_t aars_count;
  char **jars;
  size_t jars_count;
} ccb_maven_result;

void ccb_maven_result_free(ccb_maven_result *r);

// Parse a `group:artifact:version` coordinate. Returns 1 and fills
// malloc'd *group, *artifact, *version on success; returns 0 without
// touching the outputs when the coordinate is malformed (wrong field
// count, empty field, or a path-unsafe character).
int ccb_maven_parse_coord(const char *s, char **group, char **artifact, char **version);

// Resolve every `[deps] maven` coordinate (direct + transitive) into
// cache paths. prefetch != 0 downloads missing POMs/artifacts (setup
// only); prefetch == 0 reads the cache and fails on a miss. verbose
// prints nearest-wins conflict notes. readonly (explain/dry-run)
// suppresses sidecar adoption writes. On failure returns
// CCB_ERR_TOOLCHAIN with diagnostics (CCB-E3019 unresolvable,
// CCB-E3020 checksum mismatch, CCB-E3021 not prefetched) and *out is
// left empty; on success returns CCB_OK.
ccb_status ccb_maven_resolve(const ccb_config *cfg, int prefetch, int verbose,
  int readonly, ccb_maven_result *out, ccb_diag_list *diags);

// Maven cache root: $XDG_CACHE_HOME/ccb/maven2, else
// ~/.cache/ccb/maven2. Honors CCB_MAVEN_CACHE when set (tests and
// custom cache locations). NULL when no base is available; the
// caller frees the result.
char *ccb_maven_cache_dir(void);

#endif
