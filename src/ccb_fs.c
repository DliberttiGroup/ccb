// ccb_fs implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

char *ccb_path_join(const char *a, const char *b) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  size_t n = strlen(a);
  if (n > 0 && a[n - 1] != '/') {
    ccb_str_append_c(&s, '/');
  }
  ccb_str_append(&s, b);
  return ccb_str_steal(&s);
}

char *ccb_path_join3(const char *a, const char *b, const char *c) {
  char *ab = ccb_path_join(a, b);
  char *abc = ccb_path_join(ab, c);
  free(ab);
  return abc;
}

char *ccb_dirname(const char *path) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    return ccb_xstrdup(".");
  }
  if (slash == path) {
    return ccb_xstrdup("/");
  }
  return ccb_xstrndup(path, (size_t)(slash - path));
}

char *ccb_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return ccb_xstrdup(slash == NULL ? path : slash + 1);
}

char *ccb_expand_home(const char *path) {
  if (path[0] != '~' || (path[1] != '/' && path[1] != '\0')) {
    return ccb_xstrdup(path);
  }
  const char *home = getenv("HOME");
  if (home == NULL) {
    return ccb_xstrdup(path);
  }
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, home);
  ccb_str_append(&s, path + 1);
  return ccb_str_steal(&s);
}

int ccb_path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

int ccb_path_is_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return 0;
  }
  return S_ISDIR(st.st_mode);
}

ccb_status ccb_mkdir_p(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return CCB_ERR_IO;
  }
  char *tmp = ccb_xstrdup(path);
  for (char *p = tmp + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        free(tmp);
        return CCB_ERR_IO;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
    free(tmp);
    return CCB_ERR_IO;
  }
  free(tmp);
  return CCB_OK;
}

ccb_status ccb_read_file(const char *path, char **out, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return CCB_ERR_IO;
  }
  ccb_str s;
  ccb_str_init(&s);
  char chunk[8192];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    ccb_str_append_n(&s, chunk, n);
  }
  int bad = ferror(f) ? 1 : 0;
  fclose(f);
  if (bad != 0) {
    ccb_str_free(&s);
    return CCB_ERR_IO;
  }
  *out_len = s.len;
  *out = ccb_str_steal(&s);
  return CCB_OK;
}

ccb_status ccb_write_file(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return CCB_ERR_IO;
  }
  size_t wrote = len == 0 ? 0 : fwrite(data, 1, len, f);
  int bad = 0;
  if (wrote != len) {
    bad = 1;
  }
  if (fclose(f) != 0) {
    bad = 1;
  }
  return bad != 0 ? CCB_ERR_IO : CCB_OK;
}

static ccb_status ccb_remove_inner(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return CCB_ERR_IO;
  }
  if (!S_ISDIR(st.st_mode)) {
    return unlink(path) == 0 ? CCB_OK : CCB_ERR_IO;
  }
  DIR *d = opendir(path);
  if (d == NULL) {
    return CCB_ERR_IO;
  }
  ccb_status rc = CCB_OK;
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    char *child = ccb_path_join(path, e->d_name);
    if (ccb_remove_inner(child) != CCB_OK) {
      rc = CCB_ERR_IO;
    }
    free(child);
  }
  closedir(d);
  if (rmdir(path) != 0) {
    rc = CCB_ERR_IO;
  }
  return rc;
}

ccb_status ccb_remove_recursive(const char *path) {
  if (!ccb_path_exists(path)) {
    return CCB_OK;
  }
  return ccb_remove_inner(path);
}

typedef struct {
  char **items;
  size_t count;
  size_t cap;
  const char *ext;
} ccb_collect_ctx;

static void ccb_collect_add(ccb_collect_ctx *ctx, const char *path) {
  if (ctx->count == ctx->cap) {
    size_t ncap = ctx->cap == 0 ? 32 : ctx->cap * 2;
    ctx->items = ccb_xrealloc(ctx->items, ncap * sizeof(*ctx->items));
    ctx->cap = ncap;
  }
  ctx->items[ctx->count++] = ccb_xstrdup(path);
}

static int ccb_collect_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static ccb_status ccb_collect_walk(const char *dir, ccb_collect_ctx *ctx) {
  DIR *d = opendir(dir);
  if (d == NULL) {
    return CCB_ERR_IO;
  }
  struct dirent *e = NULL;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    char *child = ccb_path_join(dir, e->d_name);
    struct stat st;
    if (stat(child, &st) != 0) {
      free(child);
      closedir(d);
      return CCB_ERR_IO;
    }
    if (S_ISDIR(st.st_mode)) {
      ccb_status rc = ccb_collect_walk(child, ctx);
      free(child);
      if (rc != CCB_OK) {
        closedir(d);
        return rc;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (ctx->ext == NULL) {
        ccb_collect_add(ctx, child);
      } else {
        size_t nl = strlen(e->d_name);
        size_t el = strlen(ctx->ext);
        if (nl >= el && strcmp(e->d_name + nl - el, ctx->ext) == 0) {
          ccb_collect_add(ctx, child);
        }
      }
      free(child);
    } else {
      free(child);
    }
  }
  closedir(d);
  return CCB_OK;
}

ccb_status ccb_collect_files(const char *root, const char *ext, char ***out, size_t *out_count) {
  ccb_collect_ctx ctx;
  ctx.items = NULL;
  ctx.count = 0;
  ctx.cap = 0;
  ctx.ext = ext;
  if (!ccb_path_exists(root)) {
    *out = NULL;
    *out_count = 0;
    return CCB_OK;
  }
  if (!ccb_path_is_dir(root)) {
    *out = NULL;
    *out_count = 0;
    return CCB_OK;
  }
  ccb_status rc = ccb_collect_walk(root, &ctx);
  if (rc != CCB_OK) {
    ccb_free_str_array(ctx.items, ctx.count);
    return rc;
  }
  if (ctx.count > 1) {
    qsort(ctx.items, ctx.count, sizeof(*ctx.items), ccb_collect_cmp);
  }
  *out = ctx.items;
  *out_count = ctx.count;
  return CCB_OK;
}

void ccb_free_str_array(char **arr, size_t count) {
  if (arr == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(arr[i]);
  }
  free(arr);
}
