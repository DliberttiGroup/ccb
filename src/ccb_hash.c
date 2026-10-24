// ccb_hash implementation: standard SHA-256.
#include "ccb_common.h"
#include "ccb_hash.h"

static const uint32_t CCB_K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t ccb_rotr(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

static void ccb_block(ccb_sha256 *c, const unsigned char *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
      ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ccb_rotr(w[i - 15], 7) ^ ccb_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ccb_rotr(w[i - 2], 17) ^ ccb_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4];
  uint32_t f = c->h[5], g = c->h[6], h = c->h[7], cc = c->h[2];
  for (int i = 0; i < 64; i++) {
    uint32_t s1 = ccb_rotr(e, 6) ^ ccb_rotr(e, 11) ^ ccb_rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + s1 + ch + CCB_K[i] + w[i];
    uint32_t s0 = ccb_rotr(a, 2) ^ ccb_rotr(a, 13) ^ ccb_rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}

void ccb_sha256_init(ccb_sha256 *c) {
  c->h[0] = 0x6a09e667;
  c->h[1] = 0xbb67ae85;
  c->h[2] = 0x3c6ef372;
  c->h[3] = 0xa54ff53a;
  c->h[4] = 0x510e527f;
  c->h[5] = 0x9b05688c;
  c->h[6] = 0x1f83d9ab;
  c->h[7] = 0x5be0cd19;
  c->total = 0;
  c->buflen = 0;
}

void ccb_sha256_update(ccb_sha256 *c, const void *data, size_t len) {
  const unsigned char *p = data;
  c->total += len;
  while (len > 0) {
    size_t take = 64 - c->buflen;
    if (take > len) {
      take = len;
    }
    memcpy(c->buf + c->buflen, p, take);
    c->buflen += take;
    p += take;
    len -= take;
    if (c->buflen == 64) {
      ccb_block(c, c->buf);
      c->buflen = 0;
    }
  }
}

void ccb_sha256_final(ccb_sha256 *c, unsigned char out[32]) {
  uint64_t bits = c->total * 8;
  unsigned char pad = 0x80;
  ccb_sha256_update(c, &pad, 1);
  unsigned char zero = 0;
  while (c->buflen != 56) {
    ccb_sha256_update(c, &zero, 1);
  }
  unsigned char lenbuf[8];
  for (int i = 0; i < 8; i++) {
    lenbuf[i] = (unsigned char)(bits >> (56 - 8 * i));
  }
  // Feed the length directly without changing the total again.
  memcpy(c->buf + c->buflen, lenbuf, 8);
  ccb_block(c, c->buf);
  c->buflen = 0;
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (unsigned char)(c->h[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(c->h[i]);
  }
}

void ccb_sha256_to_hex(const unsigned char digest[32], char out[65]) {
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hexd[digest[i] >> 4];
    out[i * 2 + 1] = hexd[digest[i] & 15];
  }
  out[64] = '\0';
}

void ccb_sha256_hex(const void *data, size_t len, char out[65]) {
  static const char hexd[] = "0123456789abcdef";
  ccb_sha256 c;
  unsigned char digest[32];
  ccb_sha256_init(&c);
  ccb_sha256_update(&c, data, len);
  ccb_sha256_final(&c, digest);
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hexd[digest[i] >> 4];
    out[i * 2 + 1] = hexd[digest[i] & 15];
  }
  out[64] = '\0';
}

ccb_status ccb_sha256_file(const char *path, char out_hex[65]) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return CCB_ERR_IO;
  }
  ccb_sha256 c;
  unsigned char digest[32];
  unsigned char chunk[8192];
  ccb_sha256_init(&c);
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    ccb_sha256_update(&c, chunk, n);
  }
  int err = ferror(f) ? 1 : 0;
  fclose(f);
  if (err != 0) {
    return CCB_ERR_IO;
  }
  ccb_sha256_final(&c, digest);
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out_hex[i * 2] = hexd[digest[i] >> 4];
    out_hex[i * 2 + 1] = hexd[digest[i] & 15];
  }
  out_hex[64] = '\0';
  return CCB_OK;
}

// ---------- SHA-1 (FIPS 180-1) ----------

static uint32_t ccb_sha1_rol(uint32_t v, unsigned n) {
  return (v << n) | (v >> (32 - n));
}

static void ccb_sha1_block(ccb_sha1 *c, const unsigned char *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
      ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 80; i++) {
    w[i] = ccb_sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = c->h[0];
  uint32_t b = c->h[1];
  uint32_t cc = c->h[2];
  uint32_t d = c->h[3];
  uint32_t e = c->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f = 0;
    uint32_t k = 0;
    if (i < 20) {
      f = (b & cc) | ((~b) & d);
      k = 0x5a827999u;
    } else if (i < 40) {
      f = b ^ cc ^ d;
      k = 0x6ed9eba1u;
    } else if (i < 60) {
      f = (b & cc) | (b & d) | (cc & d);
      k = 0x8f1bbcdcu;
    } else {
      f = b ^ cc ^ d;
      k = 0xca62c1d6u;
    }
    uint32_t tmp = ccb_sha1_rol(a, 5) + f + e + k + w[i];
    e = d;
    d = cc;
    cc = ccb_sha1_rol(b, 30);
    b = a;
    a = tmp;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
}

void ccb_sha1_init(ccb_sha1 *c) {
  c->h[0] = 0x67452301u;
  c->h[1] = 0xefcdab89u;
  c->h[2] = 0x98badcfeu;
  c->h[3] = 0x10325476u;
  c->h[4] = 0xc3d2e1f0u;
  c->total = 0;
  c->buflen = 0;
}

void ccb_sha1_update(ccb_sha1 *c, const void *data, size_t len) {
  const unsigned char *p = data;
  c->total += len;
  while (len > 0) {
    size_t take = 64 - c->buflen;
    if (take > len) {
      take = len;
    }
    memcpy(c->buf + c->buflen, p, take);
    c->buflen += take;
    p += take;
    len -= take;
    if (c->buflen == 64) {
      ccb_sha1_block(c, c->buf);
      c->buflen = 0;
    }
  }
}

void ccb_sha1_final(ccb_sha1 *c, unsigned char out[20]) {
  uint64_t bits = c->total * 8;
  unsigned char pad = 0x80;
  ccb_sha1_update(c, &pad, 1);
  unsigned char zero = 0;
  while (c->buflen != 56) {
    ccb_sha1_update(c, &zero, 1);
  }
  unsigned char lenbuf[8];
  for (int i = 0; i < 8; i++) {
    lenbuf[i] = (unsigned char)(bits >> (56 - 8 * i));
  }
  // Feed the length directly without touching c->total again: the
  // block call below processes the buffered final block as-is.
  memcpy(c->buf + c->buflen, lenbuf, 8);
  ccb_sha1_block(c, c->buf);
  c->buflen = 0;
  for (int i = 0; i < 5; i++) {
    out[i * 4] = (unsigned char)(c->h[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
    out[i * 4 + 3] = (unsigned char)c->h[i];
  }
}

void ccb_sha1_to_hex(const unsigned char digest[20], char out[41]) {
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 20; i++) {
    out[i * 2] = hexd[digest[i] >> 4];
    out[i * 2 + 1] = hexd[digest[i] & 15];
  }
  out[40] = '\0';
}

ccb_status ccb_sha1_file(const char *path, char out_hex[41]) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return CCB_ERR_IO;
  }
  ccb_sha1 c;
  unsigned char digest[20];
  unsigned char chunk[8192];
  ccb_sha1_init(&c);
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    ccb_sha1_update(&c, chunk, n);
  }
  int err = ferror(f) ? 1 : 0;
  fclose(f);
  if (err != 0) {
    return CCB_ERR_IO;
  }
  ccb_sha1_final(&c, digest);
  ccb_sha1_to_hex(digest, out_hex);
  return CCB_OK;
}
