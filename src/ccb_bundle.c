// ccb_bundle: hand-encoded BundleConfig.pb (see ccb_bundle.h).
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_bundle.h"

// Append a protobuf varint (base-128, little-endian groups of 7 bits).
// Returns 0 on success, -1 if the output would overflow (cannot happen
// for size_t values, but the check keeps the arithmetic honest).
static int ccb_varint_append(uint8_t *buf, size_t cap, size_t *pos, size_t v) {
  do {
    if (*pos >= cap) {
      return -1;
    }
    uint8_t b = (uint8_t)(v & 0x7Fu);
    v >>= 7;
    if (v != 0) {
      b |= 0x80u;
    }
    buf[(*pos)++] = b;
  } while (v != 0);
  return 0;
}

// Length of a varint encoding of v.
static size_t ccb_varint_len(size_t v) {
  size_t n = 1;
  while (v >= 0x80u) {
    v >>= 7;
    n++;
  }
  return n;
}

ccb_status ccb_bundle_config_encode(const char *version, uint8_t **out, size_t *out_len) {
  if (version == NULL || out == NULL || out_len == NULL) {
    return CCB_ERR_IO;
  }
  size_t vlen = strlen(version);
  // Inner message: tag (1) + varint(vlen) + version bytes.
  size_t inner_len = 0;
  if (__builtin_add_overflow(1, ccb_varint_len(vlen), &inner_len) ||
      __builtin_add_overflow(inner_len, vlen, &inner_len)) {
    return CCB_ERR_IO;
  }
  // Outer message: tag (1) + varint(inner_len) + inner bytes.
  size_t total = 0;
  if (__builtin_add_overflow(1, ccb_varint_len(inner_len), &total) ||
      __builtin_add_overflow(total, inner_len, &total)) {
    return CCB_ERR_IO;
  }
  uint8_t *buf = ccb_xmalloc(total);
  size_t pos = 0;
  buf[pos++] = 0x0Au;  // field 1, wire type 2 (outer)
  if (ccb_varint_append(buf, total, &pos, inner_len) != 0) {
    free(buf);
    return CCB_ERR_IO;
  }
  buf[pos++] = 0x12u;  // field 2, wire type 2 (inner version)
  if (ccb_varint_append(buf, total, &pos, vlen) != 0) {
    free(buf);
    return CCB_ERR_IO;
  }
  memcpy(buf + pos, version, vlen);
  pos += vlen;
  if (pos != total) {
    free(buf);
    return CCB_ERR_IO;
  }
  *out = buf;
  *out_len = total;
  return CCB_OK;
}

// Read one varint. Returns 0 on success, -1 on truncation/overflow.
static int ccb_varint_read(const uint8_t *data, size_t len, size_t *pos, size_t *v) {
  size_t result = 0;
  int shift = 0;
  while (*pos < len) {
    uint8_t b = data[(*pos)++];
    if (shift == (int)(sizeof(size_t) * 8 - 7) && b >= 0x02u) {
      return -1;  // would overflow size_t
    }
    result |= (size_t)(b & 0x7Fu) << shift;
    if ((b & 0x80u) == 0) {
      *v = result;
      return 0;
    }
    shift += 7;
    if (shift >= (int)(sizeof(size_t) * 8)) {
      return -1;
    }
  }
  return -1;  // truncated
}

ccb_status ccb_bundle_config_decode_version(const void *data, size_t len, char **version_out) {
  if (data == NULL || version_out == NULL) {
    return CCB_ERR_IO;
  }
  const uint8_t *buf = data;
  size_t pos = 0;
  size_t outer_len = 0;
  // Outer: tag 0x0A, length-delimited bundletool message.
  if (pos >= len || buf[pos++] != 0x0Au) {
    return CCB_ERR_IO;
  }
  if (ccb_varint_read(buf, len, &pos, &outer_len) != 0) {
    return CCB_ERR_IO;
  }
  if (outer_len > len - pos) {
    return CCB_ERR_IO;
  }
  size_t inner_end = pos + outer_len;
  // Inner: tag 0x12, length-delimited version string.
  if (pos >= inner_end || buf[pos++] != 0x12u) {
    return CCB_ERR_IO;
  }
  size_t vlen = 0;
  if (ccb_varint_read(buf, inner_end, &pos, &vlen) != 0) {
    return CCB_ERR_IO;
  }
  if (vlen > inner_end - pos) {
    return CCB_ERR_IO;
  }
  pos += vlen;
  // Strict: the outer message must end exactly at the version string.
  if (pos != inner_end || inner_end != len) {
    return CCB_ERR_IO;
  }
  char *ver = ccb_xmalloc(vlen + 1);
  memcpy(ver, buf + pos - vlen, vlen);
  ver[vlen] = '\0';
  *version_out = ver;
  return CCB_OK;
}
