// ccb_resolved: runtime-resolved dependencies for one build.
//
// Boundary (Step 7): the validated config (ccb_config) is immutable
// after load — it holds only what ccb.toml declares. Stage 0 resolves
// Maven coordinates, path-library .aar files, and compiler-plugin
// jars into this struct, owned by ccb_pipe; pipeline steps read the
// resolved lists here, never from the config. A task graph built from
// the config alone (ccb check, tests) uses ccb_resolved_from_config,
// which copies the declared lists with nothing resolved.
#ifndef CCB_RESOLVED_H
#define CCB_RESOLVED_H

#include "ccb_common.h"
#include "ccb_config.h"

typedef struct {
  // Declared AARs + Maven-resolved AARs + built .aar path libraries,
  // in the order the deps step consumes them.
  char **aars;
  size_t aars_count;
  size_t aars_cap;
  // Declared JARs + Maven-resolved JARs, in classpath order.
  char **jars;
  size_t jars_count;
  size_t jars_cap;
  // Resolved kotlinc compiler-plugin jars (cache-only resolve).
  char **plugin_jars;
  size_t plugin_jars_count;
  size_t plugin_jars_cap;
} ccb_resolved;

void ccb_resolved_init(ccb_resolved *r);
// Copy the config's declared AARs/JARs (strdup'd); plugin jars empty.
// Used when no resolution runs (ccb check, graph tests).
void ccb_resolved_from_config(ccb_resolved *r, const ccb_config *cfg);
// Each push takes ownership of path.
void ccb_resolved_push_aar(ccb_resolved *r, char *path);
void ccb_resolved_push_jar(ccb_resolved *r, char *path);
void ccb_resolved_push_plugin_jar(ccb_resolved *r, char *path);
void ccb_resolved_free(ccb_resolved *r);

#endif
