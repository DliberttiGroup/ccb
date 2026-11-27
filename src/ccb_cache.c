// ccb_cache implementation: JSON manifest load/save.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_hash.h"
#include "ccb_json.h"
#include "ccb_fs.h"
#include "ccb_cache.h"

static void ccb_cache_map_set(char ***keys, char ***vals, size_t *count, size_t *cap,
    const char *key, const char *val) {
  for (size_t i = 0; i < *count; i++) {
    if (strcmp((*keys)[i], key) == 0) {
      free((*vals)[i]);
      (*vals)[i] = ccb_xstrdup(val);
      return;
    }
  }
  if (*count == *cap) {
    size_t ncap = *cap == 0 ? 16 : *cap * 2;
    *keys = ccb_xrealloc(*keys, ncap * sizeof(**keys));
    *vals = ccb_xrealloc(*vals, ncap * sizeof(**vals));
    *cap = ncap;
  }
  (*keys)[*count] = ccb_xstrdup(key);
  (*vals)[*count] = ccb_xstrdup(val);
  (*count)++;
}

static const char *ccb_cache_map_get(char **keys, char **vals, size_t count, const char *key) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(keys[i], key) == 0) {
      return vals[i];
    }
  }
  return NULL;
}

ccb_status ccb_cache_load(const char *path, ccb_cache **out) {
  ccb_cache *c = ccb_xcalloc(1, sizeof(*c));
  *out = c;
  char *text = NULL;
  size_t len = 0;
  if (ccb_read_file(path, &text, &len) != CCB_OK) {
    return CCB_OK;
  }
  char *err = NULL;
  ccb_json_value *root = ccb_json_parse(ccb_slice_from(text), &err);
  free(text);
  if (root == NULL) {
    free(err);
    return CCB_OK;
  }
  if (root->type != CCB_JSON_OBJ) {
    ccb_json_free(root);
    return CCB_OK;
  }
  const ccb_json_value *ver = ccb_json_obj_get(root, "version");
  if (ver == NULL || ver->type != CCB_JSON_NUM || ver->num != 1) {
    ccb_json_free(root);
    return CCB_OK;
  }
  const ccb_json_value *tools = ccb_json_obj_get(root, "tools");
  if (tools != NULL && tools->type == CCB_JSON_STR) {
    c->tool_fingerprint = ccb_xstrdup(tools->str);
  }
  const ccb_json_value *steps = ccb_json_obj_get(root, "steps");
  if (steps != NULL && steps->type == CCB_JSON_OBJ) {
    for (size_t i = 0; i < steps->kcount; i++) {
      const ccb_json_value *v = steps->vals[i];
      if (v->type == CCB_JSON_STR) {
        ccb_cache_map_set(&c->step_names, &c->step_fps, &c->step_count, &c->step_cap,
          steps->keys[i], v->str);
      }
    }
  }
  const ccb_json_value *files = ccb_json_obj_get(root, "files");
  if (files != NULL && files->type == CCB_JSON_OBJ) {
    for (size_t i = 0; i < files->kcount; i++) {
      const ccb_json_value *v = files->vals[i];
      if (v->type == CCB_JSON_STR) {
        ccb_cache_map_set(&c->file_paths, &c->file_hashes, &c->file_count, &c->file_cap,
          files->keys[i], v->str);
      }
    }
  }
  ccb_json_free(root);
  return CCB_OK;
}

ccb_status ccb_cache_save(const ccb_cache *cache, const char *path) {
  ccb_str s;
  ccb_str_init(&s);
  ccb_str_append(&s, "{\n  \"version\": 1");
  ccb_str_append(&s, ",\n  \"tools\": ");
  if (cache->tool_fingerprint != NULL) {
    ccb_json_escape(&s, cache->tool_fingerprint);
  } else {
    ccb_str_append(&s, "null");
  }
  ccb_str_append(&s, ",\n  \"steps\": {");
  for (size_t i = 0; i < cache->step_count; i++) {
    if (i > 0) {
      ccb_str_append(&s, ",");
    }
    ccb_str_append(&s, "\n    ");
    ccb_json_escape(&s, cache->step_names[i]);
    ccb_str_append(&s, ": ");
    ccb_json_escape(&s, cache->step_fps[i]);
  }
  if (cache->step_count > 0) {
    ccb_str_append(&s, "\n  ");
  }
  ccb_str_append(&s, "},\n  \"files\": {");
  for (size_t i = 0; i < cache->file_count; i++) {
    if (i > 0) {
      ccb_str_append(&s, ",");
    }
    ccb_str_append(&s, "\n    ");
    ccb_json_escape(&s, cache->file_paths[i]);
    ccb_str_append(&s, ": ");
    ccb_json_escape(&s, cache->file_hashes[i]);
  }
  if (cache->file_count > 0) {
    ccb_str_append(&s, "\n  ");
  }
  ccb_str_append(&s, "}\n}\n");
  // Atomic publish: an interrupted save leaves the previous cache
  // intact instead of a torn ccb-cache.json. A corrupt cache still
  // loads as empty (see ccb_cache_load), so the worst case of any
  // save failure is rebuilding everything, never a wrong build.
  ccb_status rc = ccb_write_file_atomic(path, s.buf, s.len);
  ccb_str_free(&s);
  return rc;
}

void ccb_cache_free(ccb_cache *cache) {
  if (cache == NULL) {
    return;
  }
  free(cache->tool_fingerprint);
  ccb_free_str_array(cache->step_names, cache->step_count);
  ccb_free_str_array(cache->step_fps, cache->step_count);
  ccb_free_str_array(cache->file_paths, cache->file_count);
  ccb_free_str_array(cache->file_hashes, cache->file_count);
  free(cache);
}

const char *ccb_cache_step_fp(const ccb_cache *c, const char *step) {
  return ccb_cache_map_get(c->step_names, c->step_fps, c->step_count, step);
}

void ccb_cache_set_step(ccb_cache *c, const char *step, const char *fp) {
  ccb_cache_map_set(&c->step_names, &c->step_fps, &c->step_count, &c->step_cap, step, fp);
}

const char *ccb_cache_file_hash(const ccb_cache *c, const char *relpath) {
  return ccb_cache_map_get(c->file_paths, c->file_hashes, c->file_count, relpath);
}

void ccb_cache_set_file(ccb_cache *c, const char *relpath, const char *hash) {
  ccb_cache_map_set(&c->file_paths, &c->file_hashes, &c->file_count, &c->file_cap, relpath, hash);
}

char *ccb_cache_hash_file(const char *path) {
  char hex[65];
  if (ccb_sha256_file(path, hex) != CCB_OK) {
    return NULL;
  }
  return ccb_xstrdup(hex);
}

char *ccb_cache_fingerprint(const char **keys, const char **values, size_t count) {
  ccb_sha256 ctx;
  ccb_sha256_init(&ctx);
  for (size_t i = 0; i < count; i++) {
    ccb_sha256_update(&ctx, keys[i], strlen(keys[i]));
    ccb_sha256_update(&ctx, "=", 1);
    ccb_sha256_update(&ctx, values[i], strlen(values[i]));
    ccb_sha256_update(&ctx, "\n", 1);
  }
  unsigned char digest[32];
  ccb_sha256_final(&ctx, digest);
  char *hex = ccb_xmalloc(65);
  ccb_sha256_to_hex(digest, hex);
  return hex;
}
