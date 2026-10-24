// ccb_fs: path and file helpers. Returned strings are malloc'd and owned
// by the caller unless noted.
#ifndef CCB_FS_H
#define CCB_FS_H

#include "ccb_common.h"

char *ccb_path_join(const char *a, const char *b);
char *ccb_path_join3(const char *a, const char *b, const char *c);
char *ccb_dirname(const char *path);
char *ccb_basename(const char *path);
// Expand a leading ~/ using $HOME. Returns a malloc'd copy (or a copy of
// the input when there is nothing to expand).
char *ccb_expand_home(const char *path);
int ccb_path_exists(const char *path);
int ccb_path_is_dir(const char *path);
ccb_status ccb_mkdir_p(const char *path);
// Read a whole file. *out is malloc'd, NUL-terminated, *out_len is the
// byte count without the terminator.
ccb_status ccb_read_file(const char *path, char **out, size_t *out_len);
ccb_status ccb_write_file(const char *path, const void *data, size_t len);
ccb_status ccb_remove_recursive(const char *path);
// Collect regular files under root (recursive). When ext is non-NULL,
// only files whose name ends with ext are collected. The result is sorted
// with strcmp so builds are deterministic. Free with ccb_free_str_array.
ccb_status ccb_collect_files(const char *root, const char *ext, char ***out, size_t *out_count);
void ccb_free_str_array(char **arr, size_t count);

#endif
