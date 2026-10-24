// ccb_bundle: Android App Bundle helpers, no protobuf dependency.
// BundleConfig.pb is hand-encoded on the protobuf wire format for the
// small message bundletool expects:
//   BundleConfig { BundleTool bundletool = 1; }
//   BundleTool { string version = 2; }
// Field numbers are ground truth from bundletool 1.18.1's own output
// (its BundleConfig.pb is 0x0A <len> 0x12 <len> <version>); version is
// field 2, not 1 — encoding it as field 1 makes bundletool validate
// fail with an empty version. Both fields are length-delimited
// (wire type 2). The decoder below is the strict inverse used by the
// unit tests: it extracts bundletool.version and rejects malformed
// input.
#ifndef CCB_BUNDLE_H
#define CCB_BUNDLE_H

#include "ccb_common.h"

// Encode BundleConfig.pb for the given version string. *out is malloc'd
// and *out_len is its size. The version is informational (bundletool
// only requires it to be present); CCB writes "1.0.0".
ccb_status ccb_bundle_config_encode(const char *version, uint8_t **out, size_t *out_len);

// Extract bundletool.version from BundleConfig.pb bytes. *version_out is
// malloc'd and NUL-terminated. Malformed input (bad tags, truncated
// varints, trailing bytes, non-UTF8-safe lengths) returns CCB_ERR_IO.
ccb_status ccb_bundle_config_decode_version(const void *data, size_t len, char **version_out);

#endif
