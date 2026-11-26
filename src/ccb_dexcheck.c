// ccb_dexcheck: post-build dex check (change-dexcheck).
//
// Reads dex files directly (no external tool): the 112-byte header,
// string_ids (MUTF-8), type_ids, and class_defs. Every type the dex
// references (type_ids) is normalized first — array markers stripped,
// primitives dropped — and then compared against the defined classes
// (class_defs). Normalizing before comparing matters: a reference
// to [Lcom/example/Foo; only counts when Foo itself is undefined.
//
// A type that is only referenced from an annotation or a generic
// signature still counts: d8 keeps it in type_ids, and if the class
// is genuinely missing the runtime throws for it just the same.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_dexcheck.h"
#include "ccb_fs.h"
#include "ccb_str.h"

// Platform packages: --lib inputs, never in the dex, never a bug.
// Keep this list small and explicit. L-kotlin/ and L-androidx/ are
// deliberately NOT here: the stdlib and AAR classes are program
// inputs, so their absence is a real bug.
static const char *ccb_dexcheck_allow[] = {
  "Ljava/", "Landroid/", "Ljavax/", "Ldalvik/", "Lsun/",
  "Lcom/android/", "Lorg/xml/", "Lorg/w3c/",
};

static int ccb_dexcheck_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// Sort + dedup a string array in place. Returns the new count.
static size_t ccb_dexcheck_uniq(char **arr, size_t n) {
  if (n == 0) {
    return 0;
  }
  qsort(arr, n, sizeof(*arr), ccb_dexcheck_cmp);
  size_t out = 1;
  for (size_t i = 1; i < n; i++) {
    if (strcmp(arr[i], arr[out - 1]) != 0) {
      arr[out++] = arr[i];
    } else {
      free(arr[i]);
    }
  }
  return out;
}

static int ccb_dexcheck_allowed(const char *desc) {
  for (size_t i = 0; i < CCB_ARRAY_LEN(ccb_dexcheck_allow); i++) {
    size_t n = strlen(ccb_dexcheck_allow[i]);
    if (strncmp(desc, ccb_dexcheck_allow[i], n) == 0) {
      return 1;
    }
  }
  return 0;
}

// Normalize one type descriptor: strip leading '[' array markers;
// a lone primitive letter is not a class (return NULL). Anything
// else must be an L...; class descriptor, returned malloc'd.
// Returns NULL for primitives, or on a malformed descriptor
// (*bad set to 1).
static char *ccb_dexcheck_norm(const char *desc, int *bad) {
  while (*desc == '[') {
    desc++;
  }
  if (desc[0] != '\0' && desc[1] == '\0' &&
      strchr("VZBCSIJFD", desc[0]) != NULL) {
    return NULL;
  }
  if (desc[0] != 'L' || desc[strlen(desc) - 1] != ';') {
    *bad = 1;
    return NULL;
  }
  return ccb_xstrdup(desc);
}

// Descriptor -> dotted class name ("Lkotlin/jvm/internal/Intrinsics;"
// -> "kotlin.jvm.internal.Intrinsics"). Malloc'd.
static char *ccb_dexcheck_dotted(const char *desc) {
  size_t n = strlen(desc);
  char *out = ccb_xmalloc(n); // "L" + ";" drop, plus NUL: n - 1 bytes
  size_t w = 0;
  for (size_t i = 1; i + 1 < n; i++) {
    out[w++] = desc[i] == '/' ? '.' : desc[i];
  }
  out[w] = '\0';
  return out;
}

// Bounds-checked little-endian u32 read. Returns 0 or -1.
static int ccb_dex_rd32(const unsigned char *d, size_t len, size_t off,
    uint32_t *out) {
  if (off > len || len - off < 4) {
    return -1;
  }
  *out = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
      ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
  return 0;
}

// Bounds-checked ULEB128 read. Returns 0 or -1 (truncated/overflow).
static int ccb_dex_uleb(const unsigned char *d, size_t len, size_t off,
    uint32_t *out) {
  uint32_t r = 0;
  unsigned shift = 0;
  for (int i = 0; i < 5; i++) {
    if (off >= len) {
      return -1;
    }
    unsigned char b = d[off++];
    if (shift == 28 && b > 0x0f) {
      return -1;
    }
    r |= (uint32_t)(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      *out = r;
      return 0;
    }
    shift += 7;
  }
  return -1;
}

// Read one string_data item: ULEB128 length (UTF-16 units, not used
// for sizing — bytes are found by the NUL terminator instead, since
// non-ASCII MUTF-8 is longer in bytes) then raw bytes to NUL.
// Malloc'd, or NULL with *bad set.
static char *ccb_dex_read_string(const unsigned char *d, size_t len,
    size_t off, int *bad) {
  uint32_t n = 0;
  if (ccb_dex_uleb(d, len, off, &n) != 0) {
    *bad = 1;
    return NULL;
  }
  // Re-walk the ULEB128 to find the byte start.
  size_t p = off;
  while (p < len && (d[p] & 0x80) != 0) {
    p++;
  }
  if (p >= len) {
    *bad = 1;
    return NULL;
  }
  p++;
  size_t start = p;
  while (p < len && d[p] != '\0') {
    p++;
  }
  if (p >= len) {
    *bad = 1;
    return NULL;
  }
  return ccb_xstrndup((const char *)d + start, p - start);
}

// Appends one owned name to a growing missing list.
static void ccb_dexcheck_push(char ***missing, size_t *nmissing, size_t *cap, char *name) {
  if (*nmissing == *cap) {
    *cap = *cap == 0 ? 16 : *cap * 2;
    *missing = ccb_xrealloc(*missing, *cap * sizeof(**missing));
  }
  (*missing)[(*nmissing)++] = name;
}

// One referenced class: missing from the dex (and not on the
// allow-list) means a missing entry. defined is sorted, so the
// lookup is a binary search, not a linear scan.
static void ccb_dexcheck_check_ref(char *ref, char **defined, size_t ndefined,
    char ***missing, size_t *nmissing, size_t *cap) {
  char *key = ref;
  int found = bsearch(&key, defined, ndefined,
    sizeof(*defined), ccb_dexcheck_cmp) != NULL;
  if (!found && !ccb_dexcheck_allowed(ref)) {
    ccb_dexcheck_push(missing, nmissing, cap, ccb_dexcheck_dotted(ref));
  }
}

ccb_status ccb_dexcheck_file(const char *path, char ***missing_out,
    size_t *nmissing_out, char **err_out) {
  *missing_out = NULL;
  *nmissing_out = 0;
  *err_out = NULL;
  char *data = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &data, &len) != CCB_OK) {
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_append(&m, "cannot read dex file ");
    ccb_str_append(&m, path);
    *err_out = ccb_str_steal(&m);
    return CCB_ERR_BUILD;
  }
  const unsigned char *d = (const unsigned char *)data;
  int bad = 0;
  char *why = NULL;
  // Header: 112 bytes, "dex\n" magic, little-endian tag.
  uint32_t endian = 0;
  if (len < 112 || memcmp(d, "dex\n", 4) != 0 ||
      ccb_dex_rd32(d, len, 40, &endian) != 0 || endian != 0x12345678) {
    why = "not a dex file (bad magic or endian tag)";
  }
  uint32_t str_n = 0, str_off = 0, type_n = 0, type_off = 0;
  uint32_t cls_n = 0, cls_off = 0;
  if (why == NULL &&
      (ccb_dex_rd32(d, len, 56, &str_n) != 0 ||
       ccb_dex_rd32(d, len, 60, &str_off) != 0 ||
       ccb_dex_rd32(d, len, 64, &type_n) != 0 ||
       ccb_dex_rd32(d, len, 68, &type_off) != 0 ||
       ccb_dex_rd32(d, len, 96, &cls_n) != 0 ||
       ccb_dex_rd32(d, len, 100, &cls_off) != 0)) {
    why = "truncated dex header";
  }
  char **strs = NULL;
  char **types = NULL;
  char **defined = NULL;
  size_t ndefined = 0;
  if (why == NULL) {
    strs = ccb_xcalloc(str_n == 0 ? 1 : str_n, sizeof(*strs));
    for (uint32_t i = 0; i < str_n && why == NULL; i++) {
      uint32_t off = 0;
      if (ccb_dex_rd32(d, len, (size_t)str_off + 4 * i, &off) != 0) {
        why = "string_ids table out of bounds";
        break;
      }
      strs[i] = ccb_dex_read_string(d, len, off, &bad);
      if (bad) {
        why = "string data out of bounds";
        break;
      }
    }
  }
  if (why == NULL) {
    types = ccb_xcalloc(type_n == 0 ? 1 : type_n, sizeof(*types));
    for (uint32_t i = 0; i < type_n && why == NULL; i++) {
      uint32_t idx = 0;
      if (ccb_dex_rd32(d, len, (size_t)type_off + 4 * i, &idx) != 0 ||
          idx >= str_n) {
        why = "type_ids table out of bounds";
        break;
      }
      types[i] = strs[idx];
    }
  }
  if (why == NULL) {
    defined = ccb_xcalloc(cls_n == 0 ? 1 : cls_n, sizeof(*defined));
    for (uint32_t i = 0; i < cls_n && why == NULL; i++) {
      uint32_t cidx = 0;
      if (ccb_dex_rd32(d, len, (size_t)cls_off + 32 * i, &cidx) != 0 ||
          cidx >= type_n) {
        why = "class_defs table out of bounds";
        break;
      }
      defined[ndefined++] = types[cidx];
    }
  }
  if (why == NULL) {
    // Sort only: entries are borrowed from strs, so the dedup
    // in ccb_dexcheck_uniq (which frees duplicates) must not
    // run on this array. Duplicates are harmless for lookup.
    if (ndefined > 1) {
      qsort(defined, ndefined, sizeof(*defined), ccb_dexcheck_cmp);
    }
    // Referenced types, normalized first, then compared: an array
    // reference only counts when its element class is undefined.
    char **refs = ccb_xcalloc(type_n == 0 ? 1 : type_n, sizeof(*refs));
    size_t nrefs = 0;
    for (uint32_t i = 0; i < type_n && why == NULL; i++) {
      int nbad = 0;
      char *norm = ccb_dexcheck_norm(types[i], &nbad);
      if (nbad) {
        why = "malformed type descriptor in dex";
        break;
      }
      if (norm != NULL) {
        refs[nrefs++] = norm;
      }
    }
    if (why == NULL) {
      nrefs = ccb_dexcheck_uniq(refs, nrefs);
      char **missing = NULL;
      size_t nmissing = 0;
      size_t cap = 0;
      for (size_t i = 0; i < nrefs; i++) {
        ccb_dexcheck_check_ref(refs[i], defined, ndefined, &missing, &nmissing, &cap);
      }
      *missing_out = missing;
      *nmissing_out = nmissing;
    }
    for (size_t i = 0; i < nrefs; i++) {
      free(refs[i]);
    }
    free(refs);
  }
  // strs own the strings; types/defined only borrow them.
  for (uint32_t i = 0; i < str_n; i++) {
    free(strs[i]);
  }
  free(strs);
  free(types);
  free(defined);
  free(data);
  if (why != NULL) {
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_append(&m, "malformed dex file ");
    ccb_str_append(&m, path);
    ccb_str_append(&m, ": ");
    ccb_str_append(&m, why);
    *err_out = ccb_str_steal(&m);
    return CCB_ERR_BUILD;
  }
  return CCB_OK;
}

ccb_status ccb_dexcheck_dir(const char *dir, char ***missing_out,
    size_t *nmissing_out, char **err_out) {
  *missing_out = NULL;
  *nmissing_out = 0;
  *err_out = NULL;
  char **files = NULL;
  size_t nfiles = 0;
  if (!ccb_path_is_dir(dir) ||
      ccb_collect_files(dir, ".dex", &files, &nfiles) != CCB_OK) {
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_append(&m, "no dex files found in ");
    ccb_str_append(&m, dir);
    *err_out = ccb_str_steal(&m);
    return CCB_ERR_BUILD;
  }
  if (nfiles == 0) {
    ccb_str m;
    ccb_str_init(&m);
    ccb_str_append(&m, "no dex files found in ");
    ccb_str_append(&m, dir);
    *err_out = ccb_str_steal(&m);
    ccb_free_str_array(files, nfiles);
    return CCB_ERR_BUILD;
  }
  char **missing = NULL;
  size_t nmissing = 0;
  size_t cap = 0;
  ccb_status rc = CCB_OK;
  for (size_t i = 0; i < nfiles && rc == CCB_OK; i++) {
    char **fm = NULL;
    size_t nfm = 0;
    char *err = NULL;
    rc = ccb_dexcheck_file(files[i], &fm, &nfm, &err);
    if (rc != CCB_OK) {
      *err_out = err;
    } else {
      for (size_t j = 0; j < nfm; j++) {
        ccb_dexcheck_push(&missing, &nmissing, &cap, fm[j]);
      }
      free(fm);
    }
  }
  ccb_free_str_array(files, nfiles);
  if (rc != CCB_OK) {
    ccb_free_str_array(missing, nmissing);
    return rc;
  }
  // One sort + dedup at the end: the per-file lists arrive in
  // readdir order, so merging them here is what makes the reported
  // missing classes deterministic (and alphabetical).
  nmissing = ccb_dexcheck_uniq(missing, nmissing);
  *missing_out = missing;
  *nmissing_out = nmissing;
  return CCB_OK;
}
