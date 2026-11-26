// ccb_manifest: Android manifest subset parser + AAR manifest merger.
//
// Parses the small XML subset real manifests use (elements, attributes,
// comments, processing instructions) with no external library, and merges
// AAR library manifests into the app manifest deterministically in
// dependency order. Only these entries merge:
//   - application components: activity, service, receiver, provider,
//     activity-alias (direct children of <application>)
//   - uses-permission and uses-feature (direct children of <manifest>)
// Everything else in a library manifest (application attributes,
// <uses-sdk>, <permission> declarations, <meta-data>, <queries>, ...)
// is ignored: the app manifest owns it.
//
// Conflict rules (see requirement.md §5.2):
//   - An entry whose name already exists in the app manifest is skipped
//     with a warning; the app's version is kept.
//   - The same entry (same kind, resolved name, attributes, content)
//     from two AARs is deduplicated silently.
//   - The same name with different attributes from two AARs: the first
//     AAR wins, with a warning naming both AARs.
//   - A structurally malformed manifest is a hard error naming the file.
//   - An AAR with no manifest merges as nothing, silently.
// Names are resolved against the owning manifest's package: a leading
// "." is expanded, and a name with no dot gets the package prepended.
// Merged component names are rewritten to their resolved form in the
// output, because aapt2 would otherwise resolve them against the app
// package and change their meaning.
#ifndef CCB_MANIFEST_H
#define CCB_MANIFEST_H

#include <stddef.h>

// Warnings produced by a merge. The caller owns every string in msgs
// and frees the list with ccb_manifest_warnings_free().
typedef struct {
  char **msgs;
  size_t count;
  size_t cap;
} ccb_manifest_warnings;

// Merge AAR manifests into the app manifest.
// app_text is the app manifest XML (NUL-terminated, borrowed).
// aar_texts/aar_names are parallel borrowed arrays of length aar_count;
// a NULL aar_texts[i] means that AAR has no manifest (legal, skipped).
// On success returns malloc'd merged manifest text (the caller frees
// it) and appends any conflict warnings to warns (which may be NULL).
// On failure returns NULL and, when err_out is non-NULL, sets *err_out
// to a malloc'd message naming the offending manifest (the caller
// frees it). A merge with zero AARs returns the app text unchanged.
// The package attribute of a manifest (plain-text search for
// package="..."). Returns a malloc'd package (possibly empty) or
// NULL when the attribute is absent or unterminated. The caller
// frees it. Used for the app manifest, module manifests, and AAR
// manifests, which are all plain XML here.
char *ccb_manifest_package(const char *manifest_text);

// Entry-point class names from one manifest document: the
// <application android:name> and every component's android:name, plus
// an activity-alias's android:targetActivity, all resolved against the
// manifest's own package (falling back to fallback_pkg when the
// manifest declares none). On success returns 0 with *out/*n set to a
// malloc'd array of malloc'd names (possibly empty; free with
// ccb_free_str_array). On parse failure returns -1 with *err_out set
// to a malloc'd message (when err_out is non-NULL). A manifest with
// no <application> element contributes nothing; it is not an error.
int ccb_manifest_entry_points(const char *text, const char *fallback_pkg,
    char ***out, size_t *n, char **err_out);

char *ccb_manifest_merge(const char *app_text,
    const char **aar_texts, const char **aar_names, size_t aar_count,
    ccb_manifest_warnings *warns, char **err_out);

void ccb_manifest_warnings_free(ccb_manifest_warnings *warns);

#endif
