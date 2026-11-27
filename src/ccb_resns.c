// ccb_resns: cross-module resource namespacing (see ccb_resns.h).
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_resns.h"

static void resns_push_entry(ccb_res_entry **arr, size_t *n, size_t *cap,
    const char *type, const char *name) {
  for (size_t i = 0; i < *n; i++) {
    if (strcmp((*arr)[i].type, type) == 0 &&
        strcmp((*arr)[i].name, name) == 0) {
      return;
    }
  }
  if (*n == *cap) {
    *cap = *cap == 0 ? 16 : *cap * 2;
    *arr = ccb_xrealloc(*arr, *cap * sizeof(**arr));
  }
  (*arr)[*n].type = ccb_xstrdup(type);
  (*arr)[*n].name = ccb_xstrdup(name);
  (*n)++;
}

void ccb_resns_entries_free(ccb_res_entry *e, size_t n) {
  if (e == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    free(e[i].type);
    free(e[i].name);
  }
  free(e);
}

void ccb_resns_collisions_free(ccb_res_collision *c, size_t n) {
  if (c == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    free(c[i].type);
    free(c[i].name);
    free(c[i].winner);
    ccb_free_str_array(c[i].losers, c[i].nlosers);
  }
  free(c);
}

// A values XML name attribute: `name="..."` or `name='...'` inside the
// tag text (up to end, exclusive). Returns a malloc'd value or NULL.
static char *resns_tag_name(const char *text, size_t end) {
  size_t i = 0;
  while (i + 5 < end) {
    if (strncmp(text + i, "name", 4) == 0 &&
        (text[i + 4] == '=' || text[i + 4] == ' ' || text[i + 4] == '\t')) {
      size_t j = i + 4;
      while (j < end && (text[j] == ' ' || text[j] == '\t')) {
        j++;
      }
      if (j < end && text[j] == '=') {
        j++;
        while (j < end && (text[j] == ' ' || text[j] == '\t')) {
          j++;
        }
        if (j < end && (text[j] == '"' || text[j] == '\'')) {
          char q = text[j++];
          size_t start = j;
          while (j < end && text[j] != q) {
            j++;
          }
          return ccb_xstrndup(text + start, j - start);
        }
      }
      return NULL;
    }
    i++;
  }
  return NULL;
}

// Scan one values XML file for depth-1 elements with a name attribute.
// Depth is tracked textually: comments, processing instructions, and
// declarations never change it; a self-closing tag opens and closes at
// once. Only elements opened at depth 1 are recorded.
static void resns_scan_values(const char *text, ccb_res_entry **out,
    size_t *n, size_t *cap) {
  size_t len = strlen(text);
  size_t depth = 0;
  size_t i = 0;
  while (i < len) {
    if (text[i] != '<') {
      i++;
      continue;
    }
    if (i + 3 < len && strncmp(text + i, "<!--", 4) == 0) {
      const char *end = strstr(text + i + 4, "-->");
      i = end == NULL ? len : (size_t)(end - text) + 3;
      continue;
    }
    if (i + 1 < len && (text[i + 1] == '?' || text[i + 1] == '!')) {
      const char *end = strchr(text + i + 2, '>');
      i = end == NULL ? len : (size_t)(end - text) + 1;
      continue;
    }
    if (i + 1 < len && text[i + 1] == '/') {
      const char *end = strchr(text + i + 2, '>');
      i = end == NULL ? len : (size_t)(end - text) + 1;
      if (depth > 0) {
        depth--;
      }
      continue;
    }
    size_t j = i + 1;
    while (j < len && text[j] != '>' && text[j] != ' ' &&
        text[j] != '\t' && text[j] != '\n' && text[j] != '\r' &&
        text[j] != '/') {
      j++;
    }
    if (j == i + 1) {
      i++;
      continue;
    }
    const char *end = strchr(text + j, '>');
    if (end == NULL) {
      return;
    }
    size_t tag_end = (size_t)(end - text);
    int self_close = tag_end > j && text[tag_end - 1] == '/';
    if (depth == 1) {
      char *tag = ccb_xstrndup(text + i + 1, j - (i + 1));
      char *name = resns_tag_name(text + j, tag_end - j);
      if (name != NULL) {
        resns_push_entry(out, n, cap, tag, name);
        free(name);
      }
      free(tag);
    }
    if (!self_close) {
      depth++;
    }
    i = tag_end + 1;
  }
}

static int resns_is_values_dir(const char *dir) {
  if (strncmp(dir, "values", 6) != 0) {
    return 0;
  }
  return dir[6] == '\0' || dir[6] == '-';
}

ccb_status ccb_resns_scan_dir(const char *dir, ccb_res_entry **out,
    size_t *n_out) {
  *out = NULL;
  *n_out = 0;
  if (dir == NULL || !ccb_path_is_dir(dir)) {
    return CCB_OK;
  }
  char **files = NULL;
  size_t nfiles = 0;
  if (ccb_collect_files(dir, NULL, &files, &nfiles) != CCB_OK) {
    return CCB_ERR_IO;
  }
  ccb_res_entry *entries = NULL;
  size_t n = 0;
  size_t cap = 0;
  size_t dlen = strlen(dir);
  ccb_status rc = CCB_OK;
  for (size_t i = 0; i < nfiles; i++) {
    const char *rel = files[i] + dlen + 1;
    const char *slash = strchr(rel, '/');
    if (slash == NULL) {
      continue;
    }
    char *type_dir = ccb_xstrndup(rel, (size_t)(slash - rel));
    if (resns_is_values_dir(type_dir)) {
      char *text = NULL;
      size_t tlen = 0;
      if (ccb_read_file(files[i], &text, &tlen) != CCB_OK) {
        free(type_dir);
        rc = CCB_ERR_IO;
        break;
      }
      resns_scan_values(text, &entries, &n, &cap);
      free(text);
    } else {
      char *dash = strchr(type_dir, '-');
      if (dash != NULL) {
        *dash = '\0';
      }
      const char *base = slash + 1;
      const char *dot = strrchr(base, '.');
      size_t nlen = dot == NULL ? strlen(base) : (size_t)(dot - base);
      char *name = ccb_xstrndup(base, nlen);
      resns_push_entry(&entries, &n, &cap, type_dir, name);
      free(name);
    }
    free(type_dir);
  }
  ccb_free_str_array(files, nfiles);
  if (rc != CCB_OK) {
    ccb_resns_entries_free(entries, n);
    return rc;
  }
  *out = entries;
  *n_out = n;
  return CCB_OK;
}

// One seen entry: first source index + its collision record index.
typedef struct {
  char *type;
  char *name;
  size_t first;
  size_t coll;
} resns_seen;

ccb_status ccb_resns_collisions(char **src_names,
    ccb_res_entry **src_entries, size_t *src_counts, size_t nsrc,
    ccb_res_collision **out, size_t *n_out) {
  *out = NULL;
  *n_out = 0;
  resns_seen *seen = NULL;
  size_t nseen = 0;
  size_t seen_cap = 0;
  ccb_res_collision *colls = NULL;
  size_t ncolls = 0;
  size_t colls_cap = 0;
  for (size_t s = 0; s < nsrc; s++) {
    for (size_t e = 0; e < src_counts[s]; e++) {
      const char *type = src_entries[s][e].type;
      const char *name = src_entries[s][e].name;
      size_t found = nseen;
      for (size_t k = 0; k < nseen; k++) {
        if (strcmp(seen[k].type, type) == 0 &&
            strcmp(seen[k].name, name) == 0) {
          found = k;
          break;
        }
      }
      if (found == nseen) {
        if (nseen == seen_cap) {
          seen_cap = seen_cap == 0 ? 16 : seen_cap * 2;
          seen = ccb_xrealloc(seen, seen_cap * sizeof(*seen));
        }
        seen[nseen].type = ccb_xstrdup(type);
        seen[nseen].name = ccb_xstrdup(name);
        seen[nseen].first = s;
        seen[nseen].coll = (size_t)-1;
        nseen++;
        continue;
      }
      if (seen[found].first == s) {
        continue;
      }
      if (seen[found].coll == (size_t)-1) {
        if (ncolls == colls_cap) {
          colls_cap = colls_cap == 0 ? 8 : colls_cap * 2;
          colls = ccb_xrealloc(colls, colls_cap * sizeof(*colls));
        }
        seen[found].coll = ncolls;
        colls[ncolls].type = ccb_xstrdup(type);
        colls[ncolls].name = ccb_xstrdup(name);
        colls[ncolls].winner = ccb_xstrdup(src_names[seen[found].first]);
        colls[ncolls].losers = NULL;
        colls[ncolls].nlosers = 0;
        ncolls++;
      }
      ccb_res_collision *c = &colls[seen[found].coll];
      int dup = 0;
      for (size_t k = 0; k < c->nlosers; k++) {
        if (strcmp(c->losers[k], src_names[s]) == 0) {
          dup = 1;
          break;
        }
      }
      if (!dup) {
        c->losers = ccb_xrealloc(c->losers,
          (c->nlosers + 1) * sizeof(*c->losers));
        c->losers[c->nlosers++] = ccb_xstrdup(src_names[s]);
      }
    }
  }
  for (size_t k = 0; k < nseen; k++) {
    free(seen[k].type);
    free(seen[k].name);
  }
  free(seen);
  *out = colls;
  *n_out = ncolls;
  return CCB_OK;
}
