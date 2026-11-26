// Tests for `ccb preview --storyboard "A,B,C"` via ccb_cli_main
// in-process, after chdir into the sample project, with stdout
// redirected to a temp file (the pattern from test_preview_flags).
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cli.h"
#include "ccb_str.h"
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

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

static char *sample_dir(void) {
  static const char *suffixes[] = {
    "/../examples/preview-sample/",
    "/examples/preview-sample/",
  };
  size_t i = 0;
  ccb_str dir;
  ccb_str_init(&dir);
  test_dir(&dir);
  for (i = 0; i < 2; i++) {
    ccb_str path;
    FILE *f = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
    ccb_str_append(&path, suffixes[i]);
    ccb_str_append(&path, "ccb.toml");
    f = fopen(path.buf != NULL ? path.buf : "", "rb");
    ccb_str_free(&path);
    if (f != NULL) {
      fclose(f);
      ccb_str_init(&path);
      ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
      ccb_str_append(&path, suffixes[i]);
      {
        char *out = ccb_xstrdup(path.buf);
        ccb_str_free(&path);
        ccb_str_free(&dir);
        return out;
      }
    }
  }
  ccb_str_free(&dir);
  return ccb_xstrdup("examples/preview-sample/");
}

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

static int count_occurrences(const char *hay, const char *needle) {
  int n = 0;
  size_t nl = strlen(needle);
  const char *p = hay;
  if (hay == NULL || nl == 0) {
    return 0;
  }
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += nl;
  }
  return n;
}

int main(void) {
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "sb");
  char *sdir = sample_dir();
  char *cap_path = NULL;
  char *out_path = NULL;
  char *text = NULL;
  int rc = -1;
  cap_path = join2(scratch, "cap.txt");
  out_path = join2(scratch, "out.txt");

  // --- Order + header lines, then the normal logic tree.
  {
    char *argv[] = { "ccb", "preview", "--ascii", "--storyboard",
      "Home,Detail", "--output", out_path, NULL };
    rc = run_cli(sdir, cap_path, 7, argv);
    CHECK(rc == 0);
    text = try_read(out_path);
    CHECK(text != NULL);
    if (text != NULL) {
      const char *h1 = strstr(text, "Journey 1/2: Home");
      const char *h2 = strstr(text, "Journey 2/2: Detail");
      const char *notes = strstr(text, "My Notes");
      const char *detail = strstr(text, "Note detail");
      const char *tree = strstr(text, "Home (entry)");
      CHECK(h1 != NULL && h2 != NULL && h1 < h2);
      CHECK(notes != NULL && h1 < notes && notes < h2);
      CHECK(detail != NULL && h2 < detail);
      CHECK(tree != NULL && h2 < tree);
      // Only the journey screens render as frames: no Form frame
      // title line, but the full logic tree still names Form.
      CHECK(strstr(text, "Form") != NULL);
      free(text);
    }
  }
  // --- Repeats render twice, in journey order.
  {
    char *argv[] = { "ccb", "preview", "--ascii", "--storyboard",
      "Home,Detail,Home", "--output", out_path, NULL };
    rc = run_cli(sdir, cap_path, 7, argv);
    CHECK(rc == 0);
    text = try_read(out_path);
    CHECK(text != NULL);
    if (text != NULL) {
      CHECK(strstr(text, "Journey 1/3: Home") != NULL);
      CHECK(strstr(text, "Journey 2/3: Detail") != NULL);
      CHECK(strstr(text, "Journey 3/3: Home") != NULL);
      CHECK(count_occurrences(text, "My Notes") == 2);
      free(text);
    }
  }
  // --- Whitespace around names is trimmed.
  {
    char *argv[] = { "ccb", "preview", "--ascii", "--storyboard",
      "Home, Detail", "--output", out_path, NULL };
    rc = run_cli(sdir, cap_path, 7, argv);
    CHECK(rc == 0);
    text = try_read(out_path);
    CHECK(text != NULL);
    if (text != NULL) {
      CHECK(strstr(text, "Journey 2/2: Detail") != NULL);
      free(text);
    }
  }
  // --- Errors (exit 2): unknown name, empty list, empty element,
  // whitespace-only element.
  {
    char *argv1[] = { "ccb", "preview", "--storyboard", "Nope", NULL };
    char *argv2[] = { "ccb", "preview", "--storyboard", "", NULL };
    char *argv3[] = { "ccb", "preview", "--storyboard", "Home,,Detail",
      NULL };
    char *argv4[] = { "ccb", "preview", "--storyboard", "Home, ,Detail",
      NULL };
    CHECK(run_cli(sdir, cap_path, 4, argv1) == 2);
    CHECK(run_cli(sdir, cap_path, 4, argv2) == 2);
    CHECK(run_cli(sdir, cap_path, 4, argv3) == 2);
    CHECK(run_cli(sdir, cap_path, 4, argv4) == 2);
  }
  // --- Conflicts (exit 2): --screen, positional screen, --walk,
  // --watch, --diff, --update-goldens.
  {
    char *argv1[] = { "ccb", "preview", "--storyboard", "Home",
      "--screen", "Detail", NULL };
    char *argv2[] = { "ccb", "preview", "--storyboard", "Home",
      "Detail", NULL };
    char *argv3[] = { "ccb", "preview", "--storyboard", "Home",
      "--walk", NULL };
    char *argv4[] = { "ccb", "preview", "--storyboard", "Home",
      "--watch", NULL };
    char *argv5[] = { "ccb", "preview", "--storyboard", "Home",
      "--diff", "/tmp", NULL };
    char *argv6[] = { "ccb", "preview", "--storyboard", "Home",
      "--update-goldens", NULL };
    CHECK(run_cli(sdir, cap_path, 6, argv1) == 2);
    CHECK(run_cli(sdir, cap_path, 5, argv2) == 2);
    CHECK(run_cli(sdir, cap_path, 5, argv3) == 2);
    CHECK(run_cli(sdir, cap_path, 5, argv4) == 2);
    CHECK(run_cli(sdir, cap_path, 6, argv5) == 2);
    CHECK(run_cli(sdir, cap_path, 5, argv6) == 2);
  }
  // --- --json: screens filtered to the journey, in journey order.
  {
    char *argv[] = { "ccb", "preview", "--storyboard", "Detail,Home",
      "--json", "--output", out_path, NULL };
    rc = run_cli(sdir, cap_path, 7, argv);
    CHECK(rc == 0);
    text = try_read(out_path);
    CHECK(text != NULL);
    if (text != NULL) {
      const char *detail = strstr(text, "\"name\":\"Detail\"");
      const char *home = strstr(text, "\"name\":\"Home\"");
      CHECK(detail != NULL && home != NULL && detail < home);
      CHECK(strstr(text, "\"name\":\"Form\"") == NULL);
      CHECK(strstr(text, "\"name\":\"Empty\"") == NULL);
      CHECK(strstr(text, "\"version\":2") != NULL);
      free(text);
    }
  }
  // --- --output: stdout stays empty, the storyboard is in the file.
  {
    char *argv[] = { "ccb", "preview", "--ascii", "--storyboard",
      "Detail", "--output", out_path, NULL };
    rc = run_cli(sdir, cap_path, 7, argv);
    char *cap = NULL;
    CHECK(rc == 0);
    cap = try_read(cap_path);
    CHECK(cap != NULL && cap[0] == '\0');
    free(cap);
    text = try_read(out_path);
    CHECK(text != NULL);
    if (text != NULL) {
      CHECK(strstr(text, "Journey 1/1: Detail") != NULL);
      free(text);
    }
  }

  free(cap_path);
  free(out_path);
  free(sdir);
  TEST_MAIN_END();
}
