// ccb_hash: SHA-256 (FIPS 180-4), implemented in-tree so the cache can
// hash files without any external dependency.
#ifndef CCB_HASH_H
#define CCB_HASH_H

#include "ccb_common.h"

typedef struct {
  uint32_t h[8];
  uint64_t total;
  unsigned char buf[64];
  size_t buflen;
} ccb_sha256;

void ccb_sha256_init(ccb_sha256 *c);
void ccb_sha256_update(ccb_sha256 *c, const void *data, size_t len);
void ccb_sha256_final(ccb_sha256 *c, unsigned char out[32]);
void ccb_sha256_hex(const void *data, size_t len, char out[65]);
// Render a raw digest as 64 lowercase hex chars + NUL.
void ccb_sha256_to_hex(const unsigned char digest[32], char out[65]);
// Hash a whole file. Returns CCB_OK and fills out_hex, or CCB_ERR_IO.
ccb_status ccb_sha256_file(const char *path, char out_hex[65]);

// SHA-1 (FIPS 180-1), same shape as the SHA-256 API above. Only used
// to verify repository-published .sha1 checksums for Maven artifacts;
// SHA-1 is not used for any security decision beyond cache integrity.
typedef struct {
  uint32_t h[5];
  uint64_t total;
  unsigned char buf[64];
  size_t buflen;
} ccb_sha1;

void ccb_sha1_init(ccb_sha1 *c);
void ccb_sha1_update(ccb_sha1 *c, const void *data, size_t len);
void ccb_sha1_final(ccb_sha1 *c, unsigned char out[20]);
void ccb_sha1_to_hex(const unsigned char digest[20], char out[41]);
// Hash a whole file. Returns CCB_OK and fills out_hex, or CCB_ERR_IO.
ccb_status ccb_sha1_file(const char *path, char out_hex[41]);

#endif
