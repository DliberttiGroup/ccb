// Tests for the CLI wiring owned by the coordinator: preview --diff
// conflicts and behavior, and the check/build --preview-gate flag
// conflict. Gate logic itself is covered by test_preview_gate.c and
// diff logic by test_preview_diff.c; these cases prove the flags
// reach the right code paths with the right exit codes. CLI cases run
// ccb_cli_main in-process after chdir into a scratch copy of the
// sample project, with stdout redirected to a capture file. The
// snapshot directory is a scratch dir via CCB_GOLDEN_DIR, never the
// real tests/golden/ tree.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cli.h"
#include "ccb_str.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- small file helpers ----------

static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
}

static char *try_read(const char *path) {
  FILE *f = fopen(path, "rb");
  long size = 0;
  char *buf = NULL;
  if (f == NULL) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  buf = ccb_xmalloc((size_t)size + 1);
  if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[size] = '\0';
  fclose(f);
  return buf;
}

static int write_all(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return -1;
  }
  if (len > 0 && fwrite(data, 1, (size_t)len, f) != len) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static char *find_dir(const char *suffixes[], size_t n,
    const char *probe) {
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < n; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, probe);
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    ccb_str_free(&path);
    if (f != NULL) {
      char *out = NULL;
      fclose(f);
      ccb_str_init(&path);
      ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
      ccb_str_append(&path, suffixes[i]);
      out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
      ccb_str_free(&dir);
      return out;
    }
  }
  ccb_str_free(&dir);
  return NULL;
}

static char *sample_dir(void) {
  static const char *suffixes[] = {
    "/../examples/preview-sample/",
    "/examples/preview-sample/",
  };
  char *out = find_dir(suffixes, 2, "ccb.toml");
  if (out != NULL) {
    return out;
  }
  return ccb_xstrdup("examples/preview-sample/");
}

static char *join2(const char *a, const char *b) {
  ccb_str s;
  char *out = NULL;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  ccb_str_append(&s, "/");
  ccb_str_append(&s, b);
  out = ccb_xstrdup(s.buf);
  ccb_str_free(&s);
  return out;
}

// Recursively copy a directory tree (sample project -> scratch).
static int copy_tree(const char *src, const char *dst) {
  DIR *d = opendir(src);
  struct dirent *e = NULL;
  if (d == NULL) {
    return -1;
  }
  mkdir(dst, 0777);
  while ((e = readdir(d)) != NULL) {
    char *sp = NULL;
    char *dp = NULL;
    struct stat st;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    sp = join2(src, e->d_name);
    dp = join2(dst, e->d_name);
    if (stat(sp, &st) == 0 && S_ISDIR(st.st_mode)) {
      int rc = copy_tree(sp, dp);
      free(sp);
      free(dp);
      if (rc != 0) {
        closedir(d);
        return -1;
      }
    } else {
      char *data = try_read(sp);
      int rc = data == NULL ? -1 :
        write_all(dp, data, strlen(data));
      free(data);
      free(sp);
      free(dp);
      if (rc != 0) {
        closedir(d);
        return -1;
      }
    }
  }
  closedir(d);
  return 0;
}

// ---------- CLI runner ----------

// Run ccb_cli_main with stdout redirected to cap_path, from workdir.
// Returns the CLI exit code; the caller reads cap_path afterwards.
// The process cwd is restored before returning.
static int run_cli(const char *workdir, const char *cap_path,
    int argc, char **argv) {
  char cwd[PATH_MAX];
  int saved = -1;
  int fd = -1;
  int rc = -1;
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    return -1;
  }
  if (chdir(workdir) != 0) {
    return -1;
  }
  fd = open(cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    { int rc2 = chdir(cwd); (void)rc2; }
    return -1;
  }
  saved = dup(1);
  if (saved < 0) {
    close(fd);
    { int rc2 = chdir(cwd); (void)rc2; }
    return -1;
  }
  fflush(stdout);
  dup2(fd, 1);
  close(fd);
  rc = ccb_cli_main(argc, argv);
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  { int rc2 = chdir(cwd); (void)rc2; }
  return rc;
}

static int contains(const char *hay, const char *needle) {
  return hay != NULL && needle != NULL && strstr(hay, needle) != NULL;
}

// Replace the first occurrence of `from` with `to` in the file at
// path; returns 0 on success.
static int replace_in_file(const char *path, const char *from,
    const char *to) {
  char *data = try_read(path);
  char *hit = NULL;
  ccb_str out;
  int rc = -1;
  if (data == NULL) {
    return -1;
  }
  hit = strstr(data, from);
  if (hit == NULL) {
    free(data);
    return -1;
  }
  ccb_str_init(&out);
  ccb_str_append_n(&out, data, (size_t)(hit - data));
  ccb_str_append(&out, to);
  ccb_str_append(&out, hit + strlen(from));
  rc = write_all(path, out.buf != NULL ? out.buf : "", strlen(out.buf));
  ccb_str_free(&out);
  free(data);
  return rc;
}

int main(void) {
  char *sdir = sample_dir();
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "gatediff");
  char *proj = NULL;
  char *snap = NULL;
  char *cap = NULL;
  char *out = NULL;
  proj = join2(scratch, "proj");
  snap = join2(scratch, "snap");
  cap = join2(scratch, "cap.txt");
  out = join2(scratch, "diff-out.txt");
  CHECK(copy_tree(sdir, proj) == 0);
  mkdir(snap, 0777);

  // --- Flag conflicts fail fast with exit 2, before any work. ---
  {
    char *argv[] = { "ccb", "preview", "--diff", snap, "--walk", NULL };
    CHECK(run_cli(proj, cap, 5, argv) == 2);
  }
  {
    char *argv[] = { "ccb", "preview", "--diff", snap, "--watch", NULL };
    CHECK(run_cli(proj, cap, 5, argv) == 2);
  }
  {
    char *argv[] = { "ccb", "preview", "--diff", snap,
      "--update-goldens", NULL };
    CHECK(run_cli(proj, cap, 5, argv) == 2);
  }
  {
    char *argv[] = { "ccb", "check", "--preview-gate",
      "--no-preview-gate", NULL };
    CHECK(run_cli(proj, cap, 4, argv) == 2);
  }
  {
    char *argv[] = { "ccb", "build", "--preview-gate",
      "--no-preview-gate", NULL };
    CHECK(run_cli(proj, cap, 4, argv) == 2);
  }

  // --- Snapshot the sample app into the scratch dir, then diff. ---
  setenv("CCB_GOLDEN_DIR", snap, 1);
  {
    char *argv[] = { "ccb", "preview", "--update-goldens", NULL };
    CHECK(run_cli(proj, cap, 3, argv) == 0);
  }
  unsetenv("CCB_GOLDEN_DIR");
  {
    char *argv[] = { "ccb", "preview", "--diff", snap, NULL };
    char *text = NULL;
    CHECK(run_cli(proj, cap, 4, argv) == 0);
    text = try_read(cap);
    CHECK(contains(text, "preview identical to snapshot"));
    free(text);
  }

  // --- One label change: Home frame (and JSON) differ, Detail does
  // not, so a scoped diff to Detail still passes. ---
  {
    char *flow = join2(proj, "flow");
    char *home = join2(flow, "Home.kt");
    CHECK(replace_in_file(home, "My Notes", "My Notes!") == 0);
    free(home);
    free(flow);
  }
  {
    char *argv[] = { "ccb", "preview", "--diff", snap, NULL };
    char *text = NULL;
    CHECK(run_cli(proj, cap, 4, argv) == 1);
    text = try_read(cap);
    CHECK(contains(text, "Home.txt"));
    CHECK(contains(text, "differing files"));
    free(text);
  }
  {
    char *argv[] = { "ccb", "preview", "--diff", snap, "--json", NULL };
    char *text = NULL;
    CHECK(run_cli(proj, cap, 5, argv) == 1);
    text = try_read(cap);
    CHECK(contains(text, "\"event\":\"preview-diff\""));
    CHECK(contains(text, "\"identical\":false"));
    CHECK(contains(text, "Home.txt"));
    free(text);
  }
  {
    char *argv[] = { "ccb", "preview", "--diff", snap, "--screen",
      "Detail", NULL };
    CHECK(run_cli(proj, cap, 6, argv) == 0);
  }
  {
    char *argv[] = { "ccb", "preview", "--diff", snap, "--output",
      out, NULL };
    char *text = NULL;
    char *captext = NULL;
    CHECK(run_cli(proj, cap, 6, argv) == 1);
    captext = try_read(cap);
    CHECK(captext != NULL && captext[0] == '\0');
    free(captext);
    text = try_read(out);
    CHECK(contains(text, "Home.txt"));
    free(text);
  }

  // --- Unreadable snapshot dir is a clean exit 2. ---
  {
    char *missing = join2(scratch, "nope");
    char *argv[] = { "ccb", "preview", "--diff", missing, NULL };
    CHECK(run_cli(proj, cap, 4, argv) == 2);
    free(missing);
  }

  // --- Stale snapshot file and missing snapshot file are reported,
  // not crashes. ---
  {
    char *ghost = join2(snap, "Ghost.txt");
    char *argv[] = { "ccb", "preview", "--diff", snap, NULL };
    char *text = NULL;
    CHECK(write_all(ghost, "stale\n", 6) == 0);
    CHECK(run_cli(proj, cap, 4, argv) == 1);
    text = try_read(cap);
    CHECK(contains(text, "stale: Ghost.txt"));
    free(text);
    free(ghost);
  }
  {
    char *detail = join2(snap, "Detail.txt");
    char *argv[] = { "ccb", "preview", "--diff", snap, NULL };
    char *text = NULL;
    CHECK(unlink(detail) == 0);
    CHECK(run_cli(proj, cap, 4, argv) == 1);
    text = try_read(cap);
    CHECK(contains(text, "missing: Detail.txt"));
    free(text);
    free(detail);
  }

  free(sdir);
  free(proj);
  free(snap);
  free(cap);
  free(out);
  TEST_MAIN_END();
}
