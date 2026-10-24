// ccb_zip implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_zip.h"

// ---------- CRC32 (IEEE, reflected) ----------

static uint32_t ccb_crc_table[256];
static int ccb_crc_table_ready = 0;

static void ccb_crc_init(void) {
  if (ccb_crc_table_ready) {
    return;
  }
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    ccb_crc_table[i] = c;
  }
  ccb_crc_table_ready = 1;
}

uint32_t ccb_zip_crc32(const void *data, size_t len) {
  ccb_crc_init();
  const uint8_t *p = data;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c = ccb_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

// ---------- little-endian helpers ----------

static void ccb_put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void ccb_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t ccb_get16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t ccb_get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------- inflate (RFC 1951) ----------

typedef struct {
  const uint8_t *in;
  size_t in_len;
  size_t pos;
  uint32_t bitbuf;
  int bitcnt;
} ccb_bit_reader;

static int ccb_bits(ccb_bit_reader *br, int n) {
  while (br->bitcnt < n) {
    if (br->pos >= br->in_len) {
      return -1;
    }
    br->bitbuf |= (uint32_t)br->in[br->pos++] << br->bitcnt;
    br->bitcnt += 8;
  }
  int v = (int)(br->bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u)));
  br->bitbuf >>= n;
  br->bitcnt -= n;
  return v;
}

typedef struct {
  uint16_t counts[16];
  uint16_t symbols[288];
} ccb_huff;

static int ccb_huff_build(ccb_huff *h, const uint8_t *lengths, int n) {
  memset(h->counts, 0, sizeof(h->counts));
  for (int i = 0; i < n; i++) {
    if (lengths[i] > 15) {
      return -1;
    }
    h->counts[lengths[i]]++;
  }
  h->counts[0] = 0;
  uint16_t offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; len++) {
    offs[len + 1] = offs[len] + h->counts[len];
  }
  for (int i = 0; i < n; i++) {
    if (lengths[i] != 0) {
      h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    }
  }
  return 0;
}

static int ccb_huff_decode(ccb_bit_reader *br, const ccb_huff *h) {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int len = 1; len <= 15; len++) {
    int bit = ccb_bits(br, 1);
    if (bit < 0) {
      return -1;
    }
    code |= bit;
    int count = h->counts[len];
    if (code - first < count) {
      return h->symbols[index + (code - first)];
    }
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

static const uint16_t ccb_len_base[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t ccb_len_extra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t ccb_dist_base[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
  8193, 12289, 16385, 24577
};
static const uint8_t ccb_dist_extra[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int ccb_inflate_block(ccb_bit_reader *br, const ccb_huff *lit, const ccb_huff *dist,
    uint8_t *out, size_t out_len, size_t *out_pos) {
  while (1) {
    int sym = ccb_huff_decode(br, lit);
    if (sym < 0) {
      return -1;
    }
    if (sym < 256) {
      if (*out_pos >= out_len) {
        return -1;
      }
      out[(*out_pos)++] = (uint8_t)sym;
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29) {
        return -1;
      }
      int len = ccb_len_base[sym];
      int extra = ccb_len_extra[sym];
      if (extra > 0) {
        int v = ccb_bits(br, extra);
        if (v < 0) {
          return -1;
        }
        len += v;
      }
      int dsym = ccb_huff_decode(br, dist);
      if (dsym < 0 || dsym >= 30) {
        return -1;
      }
      int d = ccb_dist_base[dsym];
      int dextra = ccb_dist_extra[dsym];
      if (dextra > 0) {
        int v = ccb_bits(br, dextra);
        if (v < 0) {
          return -1;
        }
        d += v;
      }
      if ((size_t)d > *out_pos) {
        return -1;
      }
      if (*out_pos + (size_t)len > out_len) {
        return -1;
      }
      for (int i = 0; i < len; i++) {
        out[*out_pos] = out[*out_pos - (size_t)d];
        (*out_pos)++;
      }
    }
  }
}

ccb_status ccb_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
  ccb_bit_reader br;
  br.in = in;
  br.in_len = in_len;
  br.pos = 0;
  br.bitbuf = 0;
  br.bitcnt = 0;
  size_t out_pos = 0;
  while (1) {
    int final = ccb_bits(&br, 1);
    int type = ccb_bits(&br, 2);
    if (final < 0 || type < 0) {
      return CCB_ERR_IO;
    }
    if (type == 0) {
      // Stored block: align to a byte boundary.
      br.bitcnt = 0;
      br.bitbuf = 0;
      if (br.pos + 4 > br.in_len) {
        return CCB_ERR_IO;
      }
      uint16_t len = ccb_get16(br.in + br.pos);
      uint16_t nlen = ccb_get16(br.in + br.pos + 2);
      if ((uint16_t)~len != nlen) {
        return CCB_ERR_IO;
      }
      br.pos += 4;
      if (br.pos + len > br.in_len || out_pos + len > out_len) {
        return CCB_ERR_IO;
      }
      memcpy(out + out_pos, br.in + br.pos, len);
      out_pos += len;
      br.pos += len;
    } else if (type == 1 || type == 2) {
      ccb_huff lit;
      ccb_huff dist;
      if (type == 1) {
        uint8_t lit_lengths[288];
        uint8_t dist_lengths[32];
        for (int i = 0; i < 144; i++) {
          lit_lengths[i] = 8;
        }
        for (int i = 144; i < 256; i++) {
          lit_lengths[i] = 9;
        }
        for (int i = 256; i < 280; i++) {
          lit_lengths[i] = 7;
        }
        for (int i = 280; i < 288; i++) {
          lit_lengths[i] = 8;
        }
        for (int i = 0; i < 32; i++) {
          dist_lengths[i] = 5;
        }
        if (ccb_huff_build(&lit, lit_lengths, 288) != 0) {
          return CCB_ERR_IO;
        }
        if (ccb_huff_build(&dist, dist_lengths, 32) != 0) {
          return CCB_ERR_IO;
        }
      } else {
        int hlit = ccb_bits(&br, 5);
        int hdist = ccb_bits(&br, 5);
        int hclen = ccb_bits(&br, 4);
        if (hlit < 0 || hdist < 0 || hclen < 0) {
          return CCB_ERR_IO;
        }
        hlit += 257;
        hdist += 1;
        hclen += 4;
        static const uint8_t order[19] = {
          16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
        };
        uint8_t cl_lengths[19];
        memset(cl_lengths, 0, sizeof(cl_lengths));
        for (int i = 0; i < hclen; i++) {
          int v = ccb_bits(&br, 3);
          if (v < 0) {
            return CCB_ERR_IO;
          }
          cl_lengths[order[i]] = (uint8_t)v;
        }
        ccb_huff cl;
        if (ccb_huff_build(&cl, cl_lengths, 19) != 0) {
          return CCB_ERR_IO;
        }
        uint8_t lengths[288 + 32];
        memset(lengths, 0, sizeof(lengths));
        int total = hlit + hdist;
        int i = 0;
        while (i < total) {
          int sym = ccb_huff_decode(&br, &cl);
          if (sym < 0) {
            return CCB_ERR_IO;
          }
          if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
          } else if (sym == 16) {
            if (i == 0) {
              return CCB_ERR_IO;
            }
            int rep = ccb_bits(&br, 2);
            if (rep < 0) {
              return CCB_ERR_IO;
            }
            rep += 3;
            uint8_t prev = lengths[i - 1];
            while (rep-- > 0 && i < total) {
              lengths[i++] = prev;
            }
          } else if (sym == 17) {
            int rep = ccb_bits(&br, 3);
            if (rep < 0) {
              return CCB_ERR_IO;
            }
            rep += 3;
            while (rep-- > 0 && i < total) {
              lengths[i++] = 0;
            }
          } else if (sym == 18) {
            int rep = ccb_bits(&br, 7);
            if (rep < 0) {
              return CCB_ERR_IO;
            }
            rep += 11;
            while (rep-- > 0 && i < total) {
              lengths[i++] = 0;
            }
          } else {
            return CCB_ERR_IO;
          }
        }
        if (ccb_huff_build(&lit, lengths, hlit) != 0) {
          return CCB_ERR_IO;
        }
        if (ccb_huff_build(&dist, lengths + hlit, hdist) != 0) {
          return CCB_ERR_IO;
        }
      }
      if (ccb_inflate_block(&br, &lit, &dist, out, out_len, &out_pos) != 0) {
        return CCB_ERR_IO;
      }
    } else {
      return CCB_ERR_IO;
    }
    if (final) {
      break;
    }
  }
  if (out_pos != out_len) {
    return CCB_ERR_IO;
  }
  return CCB_OK;
}

// ---------- writer ----------

// Fixed DOS timestamp: 1980-01-01 00:00:00, for reproducible output.
#define CCB_ZIP_DOS_TIME 0
#define CCB_ZIP_DOS_DATE 0x21

static int ccb_zip_name_ok(const char *name) {
  if (name[0] == '\0' || name[0] == '/') {
    return 0;
  }
  const char *p = name;
  while (1) {
    const char *slash = strchr(p, '/');
    size_t seg = slash == NULL ? strlen(p) : (size_t)(slash - p);
    if (seg == 2 && p[0] == '.' && p[1] == '.') {
      return 0;
    }
    if (slash == NULL) {
      break;
    }
    p = slash + 1;
  }
  return 1;
}

ccb_status ccb_zip_writer_init(ccb_zip_writer *w, const char *path) {
  memset(w, 0, sizeof(*w));
  w->path = ccb_xstrdup(path);
  w->f = fopen(path, "wb");
  if (w->f == NULL) {
    return CCB_ERR_IO;
  }
  return CCB_OK;
}

static ccb_status ccb_zip_writer_add_entry(ccb_zip_writer *w, const char *archive_name, uint32_t crc,
    uint32_t size, uint32_t offset) {
  for (size_t i = 0; i < w->count; i++) {
    if (strcmp(w->names[i], archive_name) == 0) {
      return CCB_ERR_IO;
    }
  }
  if (w->count == w->cap) {
    size_t ncap = w->cap == 0 ? 32 : w->cap * 2;
    w->names = ccb_xrealloc(w->names, ncap * sizeof(*w->names));
    w->crcs = ccb_xrealloc(w->crcs, ncap * sizeof(*w->crcs));
    w->sizes = ccb_xrealloc(w->sizes, ncap * sizeof(*w->sizes));
    w->offsets = ccb_xrealloc(w->offsets, ncap * sizeof(*w->offsets));
    w->cap = ncap;
  }
  w->names[w->count] = ccb_xstrdup(archive_name);
  w->crcs[w->count] = crc;
  w->sizes[w->count] = size;
  w->offsets[w->count] = offset;
  w->count++;
  return CCB_OK;
}

static ccb_status ccb_zip_writer_write_header(ccb_zip_writer *w, const char *archive_name, uint32_t crc,
    uint32_t size) {
  uint8_t hdr[30];
  ccb_put32(hdr, 0x04034b50);
  ccb_put16(hdr + 4, 20);
  ccb_put16(hdr + 6, 0);
  ccb_put16(hdr + 8, 0);
  ccb_put16(hdr + 10, CCB_ZIP_DOS_TIME);
  ccb_put16(hdr + 12, CCB_ZIP_DOS_DATE);
  ccb_put32(hdr + 14, crc);
  ccb_put32(hdr + 18, size);
  ccb_put32(hdr + 22, size);
  ccb_put16(hdr + 26, (uint16_t)strlen(archive_name));
  ccb_put16(hdr + 28, 0);
  if (fwrite(hdr, 1, 30, w->f) != 30) {
    return CCB_ERR_IO;
  }
  if (fwrite(archive_name, 1, strlen(archive_name), w->f) != strlen(archive_name)) {
    return CCB_ERR_IO;
  }
  return CCB_OK;
}

ccb_status ccb_zip_writer_add_data(ccb_zip_writer *w, const char *archive_name, const void *data, size_t len) {
  if (!ccb_zip_name_ok(archive_name) || len > 0xFFFFFFFFu) {
    return CCB_ERR_IO;
  }
  long offset = ftell(w->f);
  if (offset < 0) {
    return CCB_ERR_IO;
  }
  uint32_t crc = ccb_zip_crc32(data, len);
  if (ccb_zip_writer_write_header(w, archive_name, crc, (uint32_t)len) != CCB_OK) {
    return CCB_ERR_IO;
  }
  if (len > 0 && fwrite(data, 1, len, w->f) != len) {
    return CCB_ERR_IO;
  }
  return ccb_zip_writer_add_entry(w, archive_name, crc, (uint32_t)len, (uint32_t)offset);
}

ccb_status ccb_zip_writer_add_file(ccb_zip_writer *w, const char *archive_name, const char *fs_path) {
  if (!ccb_zip_name_ok(archive_name)) {
    return CCB_ERR_IO;
  }
  FILE *in = fopen(fs_path, "rb");
  if (in == NULL) {
    return CCB_ERR_IO;
  }
  // First pass: size + CRC.
  uint32_t crc = 0;
  {
    ccb_crc_init();
    uint32_t c = 0xFFFFFFFFu;
    uint8_t buf[8192];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      for (size_t i = 0; i < n; i++) {
        c = ccb_crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
      }
    }
    if (ferror(in)) {
      fclose(in);
      return CCB_ERR_IO;
    }
    crc = c ^ 0xFFFFFFFFu;
  }
  if (fseek(in, 0, SEEK_END) != 0) {
    fclose(in);
    return CCB_ERR_IO;
  }
  long size = ftell(in);
  if (size < 0 || size > 0xFFFFFFFFL) {
    fclose(in);
    return CCB_ERR_IO;
  }
  rewind(in);
  long offset = ftell(w->f);
  if (offset < 0) {
    fclose(in);
    return CCB_ERR_IO;
  }
  if (ccb_zip_writer_write_header(w, archive_name, crc, (uint32_t)size) != CCB_OK) {
    fclose(in);
    return CCB_ERR_IO;
  }
  uint8_t buf[8192];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, w->f) != n) {
      fclose(in);
      return CCB_ERR_IO;
    }
  }
  int bad = ferror(in) ? 1 : 0;
  fclose(in);
  if (bad) {
    return CCB_ERR_IO;
  }
  return ccb_zip_writer_add_entry(w, archive_name, crc, (uint32_t)size, (uint32_t)offset);
}

ccb_status ccb_zip_writer_finish(ccb_zip_writer *w) {
  long cd_offset = ftell(w->f);
  if (cd_offset < 0) {
    return CCB_ERR_IO;
  }
  for (size_t i = 0; i < w->count; i++) {
    uint8_t hdr[46];
    size_t nl = strlen(w->names[i]);
    ccb_put32(hdr, 0x02014b50);
    ccb_put16(hdr + 4, 0x031E);
    ccb_put16(hdr + 6, 20);
    ccb_put16(hdr + 8, 0);
    ccb_put16(hdr + 10, 0);
    ccb_put16(hdr + 12, CCB_ZIP_DOS_TIME);
    ccb_put16(hdr + 14, CCB_ZIP_DOS_DATE);
    ccb_put32(hdr + 16, w->crcs[i]);
    ccb_put32(hdr + 20, w->sizes[i]);
    ccb_put32(hdr + 24, w->sizes[i]);
    ccb_put16(hdr + 28, (uint16_t)nl);
    ccb_put16(hdr + 30, 0);
    ccb_put16(hdr + 32, 0);
    ccb_put16(hdr + 34, 0);
    ccb_put16(hdr + 36, 0);
    ccb_put32(hdr + 38, 0100644u << 16);
    ccb_put32(hdr + 42, w->offsets[i]);
    if (fwrite(hdr, 1, 46, w->f) != 46) {
      return CCB_ERR_IO;
    }
    if (fwrite(w->names[i], 1, nl, w->f) != nl) {
      return CCB_ERR_IO;
    }
  }
  long cd_end = ftell(w->f);
  if (cd_end < 0) {
    return CCB_ERR_IO;
  }
  uint8_t eocd[22];
  ccb_put32(eocd, 0x06054b50);
  ccb_put16(eocd + 4, 0);
  ccb_put16(eocd + 6, 0);
  ccb_put16(eocd + 8, (uint16_t)w->count);
  ccb_put16(eocd + 10, (uint16_t)w->count);
  ccb_put32(eocd + 12, (uint32_t)(cd_end - cd_offset));
  ccb_put32(eocd + 16, (uint32_t)cd_offset);
  ccb_put16(eocd + 20, 0);
  if (fwrite(eocd, 1, 22, w->f) != 22) {
    return CCB_ERR_IO;
  }
  if (fclose(w->f) != 0) {
    w->f = NULL;
    return CCB_ERR_IO;
  }
  w->f = NULL;
  w->finished = 1;
  for (size_t i = 0; i < w->count; i++) {
    free(w->names[i]);
  }
  free(w->names);
  free(w->crcs);
  free(w->sizes);
  free(w->offsets);
  w->names = NULL;
  w->crcs = NULL;
  w->sizes = NULL;
  w->offsets = NULL;
  free(w->path);
  w->path = NULL;
  return CCB_OK;
}

void ccb_zip_writer_abort(ccb_zip_writer *w) {
  if (w->f != NULL) {
    fclose(w->f);
    w->f = NULL;
  }
  if (w->path != NULL) {
    remove(w->path);
  }
  for (size_t i = 0; i < w->count; i++) {
    free(w->names[i]);
  }
  free(w->names);
  free(w->crcs);
  free(w->sizes);
  free(w->offsets);
  free(w->path);
  memset(w, 0, sizeof(*w));
}

// ---------- reader ----------

ccb_status ccb_zip_open(const char *path, ccb_zip_archive **out) {
  *out = NULL;
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return CCB_ERR_IO;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return CCB_ERR_IO;
  }
  long file_size = ftell(f);
  if (file_size < 22) {
    fclose(f);
    return CCB_ERR_IO;
  }
  // EOCD lives in the last 64KB + 22 bytes (max comment size).
  long scan_start = file_size > (long)(65536 + 22) ? file_size - (65536 + 22) : 0;
  long scan_len = file_size - scan_start;
  uint8_t *tail = ccb_xmalloc((size_t)scan_len);
  if (fseek(f, scan_start, SEEK_SET) != 0) {
    free(tail);
    fclose(f);
    return CCB_ERR_IO;
  }
  if (fread(tail, 1, (size_t)scan_len, f) != (size_t)scan_len) {
    free(tail);
    fclose(f);
    return CCB_ERR_IO;
  }
  long eocd_off = -1;
  for (long i = scan_len - 22; i >= 0; i--) {
    if (ccb_get32(tail + i) == 0x06054b50) {
      eocd_off = scan_start + i;
      break;
    }
  }
  if (eocd_off < 0) {
    free(tail);
    fclose(f);
    return CCB_ERR_IO;
  }
  uint8_t *eocd = tail + (eocd_off - scan_start);
  uint16_t cd_count = ccb_get16(eocd + 10);
  uint32_t cd_size = ccb_get32(eocd + 12);
  uint32_t cd_offset = ccb_get32(eocd + 16);
  free(tail);
  if (fseek(f, cd_offset, SEEK_SET) != 0) {
    fclose(f);
    return CCB_ERR_IO;
  }
  ccb_zip_archive *a = ccb_xcalloc(1, sizeof(*a));
  a->path = ccb_xstrdup(path);
  a->entries = ccb_xmalloc((cd_count == 0 ? 1 : cd_count) * sizeof(*a->entries));
  for (uint16_t i = 0; i < cd_count; i++) {
    uint8_t hdr[46];
    if (fread(hdr, 1, 46, f) != 46 || ccb_get32(hdr) != 0x02014b50) {
      ccb_zip_close(a);
      fclose(f);
      return CCB_ERR_IO;
    }
    ccb_zip_entry *e = &a->entries[a->count];
    e->flags = ccb_get16(hdr + 8);
    e->method = ccb_get16(hdr + 10);
    e->crc = ccb_get32(hdr + 16);
    e->comp_size = ccb_get32(hdr + 20);
    e->size = ccb_get32(hdr + 24);
    uint16_t nl = ccb_get16(hdr + 28);
    uint16_t el = ccb_get16(hdr + 30);
    uint16_t cl = ccb_get16(hdr + 32);
    e->local_offset = ccb_get32(hdr + 42);
    if (e->comp_size == 0xFFFFFFFFu || e->size == 0xFFFFFFFFu || e->local_offset == 0xFFFFFFFFu) {
      ccb_zip_close(a);
      fclose(f);
      return CCB_ERR_IO;
    }
    if ((e->flags & 1) != 0) {
      ccb_zip_close(a);
      fclose(f);
      return CCB_ERR_IO;
    }
    e->name = ccb_xmalloc(nl + 1);
    if (fread(e->name, 1, nl, f) != nl) {
      ccb_zip_close(a);
      fclose(f);
      return CCB_ERR_IO;
    }
    e->name[nl] = '\0';
    if (fseek(f, el + cl, SEEK_CUR) != 0) {
      ccb_zip_close(a);
      fclose(f);
      return CCB_ERR_IO;
    }
    (void)cd_size;
    a->count++;
  }
  fclose(f);
  *out = a;
  return CCB_OK;
}

void ccb_zip_close(ccb_zip_archive *a) {
  if (a == NULL) {
    return;
  }
  for (size_t i = 0; i < a->count; i++) {
    free(a->entries[i].name);
  }
  free(a->entries);
  free(a->path);
  free(a);
}

const ccb_zip_entry *ccb_zip_find(const ccb_zip_archive *a, const char *name) {
  for (size_t i = 0; i < a->count; i++) {
    if (strcmp(a->entries[i].name, name) == 0) {
      return &a->entries[i];
    }
  }
  return NULL;
}

ccb_status ccb_zip_read_entry(const ccb_zip_archive *a, const ccb_zip_entry *e, void **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0;
  if (e->method != 0 && e->method != 8) {
    return CCB_ERR_IO;
  }
  FILE *f = fopen(a->path, "rb");
  if (f == NULL) {
    return CCB_ERR_IO;
  }
  if (fseek(f, e->local_offset, SEEK_SET) != 0) {
    fclose(f);
    return CCB_ERR_IO;
  }
  uint8_t hdr[30];
  if (fread(hdr, 1, 30, f) != 30 || ccb_get32(hdr) != 0x04034b50) {
    fclose(f);
    return CCB_ERR_IO;
  }
  uint16_t nl = ccb_get16(hdr + 26);
  uint16_t el = ccb_get16(hdr + 28);
  if (fseek(f, nl + el, SEEK_CUR) != 0) {
    fclose(f);
    return CCB_ERR_IO;
  }
  uint8_t *comp = ccb_xmalloc(e->comp_size == 0 ? 1 : e->comp_size);
  if (e->comp_size > 0 && fread(comp, 1, e->comp_size, f) != e->comp_size) {
    free(comp);
    fclose(f);
    return CCB_ERR_IO;
  }
  fclose(f);
  uint8_t *data = NULL;
  if (e->method == 0) {
    if (e->comp_size != e->size) {
      free(comp);
      return CCB_ERR_IO;
    }
    data = comp;
  } else {
    data = ccb_xmalloc(e->size == 0 ? 1 : e->size);
    if (ccb_inflate(comp, e->comp_size, data, e->size) != CCB_OK) {
      free(comp);
      free(data);
      return CCB_ERR_IO;
    }
    free(comp);
  }
  if (ccb_zip_crc32(data, e->size) != e->crc) {
    free(data);
    return CCB_ERR_IO;
  }
  *out = data;
  *out_len = e->size;
  return CCB_OK;
}
