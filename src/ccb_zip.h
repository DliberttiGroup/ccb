// ccb_zip: ZIP archives, written from scratch.
// The writer stores entries uncompressed (method 0) with fixed 1980
// timestamps, so builds are byte-reproducible (see M4 docs). The reader
// handles stored and deflated entries (RFC 1951 inflate, in-tree) and
// verifies CRC32. ZIP64 and encrypted entries are rejected: they are
// outside the v1 scope and must fail loudly, not silently.
#ifndef CCB_ZIP_H
#define CCB_ZIP_H

#include "ccb_common.h"

uint32_t ccb_zip_crc32(const void *data, size_t len);

// Raw DEFLATE (RFC 1951) decompression. in_len is the exact compressed
// size; out must be exactly out_len bytes when the stream ends.
ccb_status ccb_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

typedef struct {
  FILE *f;
  char *path;
  char **names;
  uint32_t *crcs;
  uint32_t *sizes;
  uint32_t *offsets;
  size_t count;
  size_t cap;
  int finished;
} ccb_zip_writer;

ccb_status ccb_zip_writer_init(ccb_zip_writer *w, const char *path);
ccb_status ccb_zip_writer_add_file(ccb_zip_writer *w, const char *archive_name, const char *fs_path);
ccb_status ccb_zip_writer_add_data(ccb_zip_writer *w, const char *archive_name, const void *data, size_t len);
ccb_status ccb_zip_writer_finish(ccb_zip_writer *w);
void ccb_zip_writer_abort(ccb_zip_writer *w);

typedef struct {
  char *name;
  uint16_t method;
  uint16_t flags;
  uint32_t crc;
  uint32_t comp_size;
  uint32_t size;
  uint32_t local_offset;
} ccb_zip_entry;

typedef struct {
  char *path;
  ccb_zip_entry *entries;
  size_t count;
} ccb_zip_archive;

ccb_status ccb_zip_open(const char *path, ccb_zip_archive **out);
void ccb_zip_close(ccb_zip_archive *a);
const ccb_zip_entry *ccb_zip_find(const ccb_zip_archive *a, const char *name);
// Decompressed bytes in a malloc'd buffer. Verifies CRC32.
ccb_status ccb_zip_read_entry(const ccb_zip_archive *a, const ccb_zip_entry *e, void **out, size_t *out_len);

#endif
