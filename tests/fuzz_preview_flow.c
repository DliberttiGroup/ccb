// fuzz_preview_flow: standalone fuzz harness for the Kotlin-structure
// parser (pv_flow_parse in src/ccb_preview_flow.c). It is NOT a unit
// test: it asserts nothing about parse results. It feeds each input
// file (or stdin, when no path is given) to the parser to completion
// and exits 0 whenever the parser returns normally, however many
// diagnostics it emits. A nonzero exit therefore means only a crash
// or a sanitizer abort -- that is the failure signal. It never writes
// to its inputs and never creates files; its only output is one "ok"
// line per input on stdout (plus unreadable-input warnings on stderr).
// Usage: fuzz_preview_flow [path ...]
// A path may be a file or a directory; a directory contributes only
// its *.kt entries, so the whole corpus dir can be passed at once.
// Inputs are read as bytes and NUL-terminated for the parser, which
// works on a NUL-terminated string (see pv_flow_parse); an embedded
// NUL therefore truncates the effective input.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_diag.h"
#include "ccb_preview.h"

#include <dirent.h>
#include <sys/stat.h>

// Read all bytes from f, NUL-terminated. Returns NULL on I/O error.
static char *read_all(FILE *f, size_t *len_out) {
  size_t cap = 4096;
  size_t len = 0;
  char *buf = ccb_xmalloc(cap + 1);
  for (;;) {
    size_t n = 0;
    if (len == cap) {
      cap *= 2;
      buf = ccb_xrealloc(buf, cap + 1);
    }
    n = fread(buf + len, 1, cap - len, f);
    len += n;
    if (n == 0) {
      break;
    }
  }
  if (ferror(f)) {
    free(buf);
    return NULL;
  }
  buf[len] = '\0';
  *len_out = len;
  return buf;
}

// True when ext appears as a dot-separated component of name, e.g.
// "foo.kt" or "foo.kt.flip3-2" (mutated inputs keep the original
// name as a prefix). A plain end-of-name check would silently skip
// every mutated file, which is exactly the input class this harness
// exists to run.
static int has_ext(const char *name, const char *ext) {
  size_t m = strlen(ext);
  const char *p = name;
  while ((p = strchr(p, '.')) != NULL) {
    if (strncmp(p + 1, ext, m) == 0 &&
        (p[1 + m] == '\0' || p[1 + m] == '.')) {
      return 1;
    }
    p++;
  }
  return 0;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// Parse one input buffer to completion. Always returns 0: diagnostics
// are expected output, not failure.
static int run_flow(const char *text, const char *label) {
  ccb_diag_list diags;
  pv_screen **screens = NULL;
  size_t nscreens = 0;
  size_t nedges = 0;
  size_t i = 0;
  size_t ndiags = 0;
  CCB_AUTO_ARENA ccb_arena *arena = ccb_arena_create();
  ccb_diag_list_init(&diags);
  (void)pv_flow_parse(arena, text, label, &diags, &screens, &nscreens);
  ndiags = diags.count;
  for (i = 0; i < nscreens; i++) {
    nedges += screens[i]->nedges;
  }
  ccb_diag_list_free(&diags);
  printf("ok %s screens=%zu edges=%zu diags=%zu\n", label, nscreens,
    nedges, ndiags);
  fflush(stdout); // progress must survive a kill/timeout mid-corpus
  return 0;
}

static int run_file(const char *path) {
  FILE *f = fopen(path, "rb");
  char *text = NULL;
  size_t len = 0;
  int rc = 0;
  if (f == NULL) {
    fprintf(stderr, "fuzz_preview_flow: cannot open %s\n", path);
    return 1;
  }
  text = read_all(f, &len);
  fclose(f);
  if (text == NULL) {
    fprintf(stderr, "fuzz_preview_flow: cannot read %s\n", path);
    return 1;
  }
  (void)len;
  rc = run_flow(text, path);
  free(text);
  return rc;
}

// Run every *.kt entry of a directory, in sorted order for stable output.
static int run_dir(const char *dir) {
  DIR *d = NULL;
  char **names = NULL;
  size_t count = 0;
  size_t cap = 0;
  size_t i = 0;
  int rc = 0;
  struct dirent *e = NULL;
  if (dir == NULL) {
    fprintf(stderr, "fuzz_preview_flow: NULL directory\n");
    return 1;
  }
  d = opendir(dir);
  if (d == NULL) {
    fprintf(stderr, "fuzz_preview_flow: cannot open directory %s\n", dir);
    return 1;
  }
  while ((e = readdir(d)) != NULL) {
    if (!has_ext(e->d_name, "kt")) {
      continue;
    }
    if (count == cap) {
      cap = cap == 0 ? 16 : cap * 2;
      names = ccb_xrealloc(names, cap * sizeof(*names));
    }
    names[count] = ccb_xstrdup(e->d_name);
    count++;
  }
  closedir(d);
  if (count == 0) {
    // Not a failure (exit stays 0), but a silent no-op run would hide
    // a mistyped directory, so say so loudly.
    fprintf(stderr, "fuzz_preview_flow: no .kt inputs in %s\n", dir);
  }
  qsort(names, count, sizeof(*names), cmp_str);
  for (i = 0; i < count; i++) {
    size_t n = strlen(dir) + 1 + strlen(names[i]) + 1;
    char *path = ccb_xmalloc(n);
    snprintf(path, n, "%s/%s", dir, names[i]);
    if (run_file(path) != 0) {
      rc = 1;
    }
    free(path);
    free(names[i]);
  }
  free(names);
  return rc;
}

static int run_path(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
    return run_dir(path);
  }
  return run_file(path);
}

int main(int argc, char **argv) {
  int i = 0;
  int rc = 0;
  if (argc < 2) {
    char *text = NULL;
    size_t len = 0;
    text = read_all(stdin, &len);
    if (text == NULL) {
      fprintf(stderr, "fuzz_preview_flow: cannot read stdin\n");
      return 1;
    }
    (void)len;
    rc = run_flow(text, "<stdin>");
    free(text);
    return rc;
  }
  for (i = 1; i < argc; i++) {
    if (run_path(argv[i]) != 0) {
      rc = 1;
    }
  }
  return rc;
}
