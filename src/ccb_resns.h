// ccb_resns: cross-module resource namespacing.
// A res/ tree contributes (type, name) entries: file-based resources
// (layout/foo.xml -> layout/foo) and values resources (<string
// name="x"> -> string/x). Given the sources in priority order (the app
// first, then libraries in merge order), a collision is an entry a later
// source redefines; the earliest source wins, because the app link
// passes libraries as aapt2 -R overlays with the app last. Entries are
// (type, name) only: a qualified variant (values-night) counts as the
// same entry, so a warning means the app's (or earliest library's)
// definition wins where both apply, not that the qualified value is
// gone. The values scan is textual: element tags and name attributes
// at depth 1 only, so <item> children of <style> never count.
#ifndef CCB_RESNS_H
#define CCB_RESNS_H

#include "ccb_common.h"

typedef struct {
  char *type; // e.g. "string", "layout", "drawable"
  char *name; // resource name without type or extension
} ccb_res_entry;

// One (type, name) defined by more than one source: winner is the
// first source in priority order, losers are the redefining sources
// (display names, in priority order).
typedef struct {
  char *type;
  char *name;
  char *winner;
  char **losers;
  size_t nlosers;
} ccb_res_collision;

// Scan a res/ dir into (type, name) entries, sorted by path (the
// collector sorts). A missing dir or NULL contributes no entries.
// Returns CCB_OK, or CCB_ERR_IO when a file cannot be read.
ccb_status ccb_resns_scan_dir(const char *dir, ccb_res_entry **out,
  size_t *n_out);
// Find collisions across sources in priority order: src_names[i] is
// the display name of src_entries[i] (src_counts[i] entries). A source
// that defines the same entry twice counts once. Collisions come out
// in order of first appearance.
ccb_status ccb_resns_collisions(char **src_names,
  ccb_res_entry **src_entries, size_t *src_counts, size_t nsrc,
  ccb_res_collision **out, size_t *n_out);
void ccb_resns_entries_free(ccb_res_entry *e, size_t n);
void ccb_resns_collisions_free(ccb_res_collision *c, size_t n);

#endif
