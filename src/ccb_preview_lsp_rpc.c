// ccb_preview_lsp_rpc: JSON-RPC framing + method helpers.
// Split from ccb_preview_lsp.c.

#include "ccb_preview_lsp.h"
#include "ccb_preview_trace.h"
#include "ccb_preview_watch.h"
#include "ccb_json.h"
#include "ccb_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ccb_preview_lsp_internal.h"

// ---------- JSON-RPC framing ----------

// Buffered stdin reader: partial headers and bodies survive across
// loop iterations (a client may split a message across writes).

static void lsp_reader_compact(lsp_reader *r) {
  if (r->pos == 0) {
    return;
  }
  if (r->pos < r->len) {
    memmove(r->buf, r->buf + r->pos, r->len - r->pos);
  }
  r->len -= r->pos;
  r->pos = 0;
}

// Read more input (non-blocking fd). Returns 1 when new bytes
// arrived, 0 on EAGAIN/EINTR/EOF/error (eof set on EOF/error).
int lsp_reader_more(lsp_reader *r) {
  ssize_t n = 0;
  lsp_reader_compact(r);
  if (r->len == r->cap) {
    size_t ncap = r->cap == 0 ? 8192 : r->cap * 2;
    r->buf = ccb_xrealloc(r->buf, ncap);
    r->cap = ncap;
  }
  n = read(r->fd, r->buf + r->len, r->cap - r->len);
  if (n > 0) {
    r->len += (size_t)n;
    return 1;
  }
  if (n == 0) {
    r->eof = 1;
    return 0;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    return 0;
  }
  r->eof = 1;
  return 0;
}

// Try to extract one complete message body. Returns 1 with a
// malloc'd NUL-terminated body in *body_io, 0 when more input is
// needed, 2 when a header block without Content-Length was skipped
// (caller retries immediately for a following message).
// The Content-Length of one header line [p, line_end): the parsed
// length, or -1 when the line is not a Content-Length header.
static long lsp_header_length(const char *buf, size_t p,
    size_t line_end) {
  size_t ll = line_end - p;
  const char *v = NULL;
  char *end = NULL;
  long n = 0;
  if (ll > 0 && buf[p + ll - 1] == '\r') {
    ll--;
  }
  if (ll < 15 || (buf[p] != 'C' && buf[p] != 'c') ||
      strncasecmp(buf + p, "content-length:", 15) != 0) {
    return -1;
  }
  v = buf + p + 15;
  while (*v == ' ' || *v == '\t') {
    v++;
  }
  n = strtol(v, &end, 10);
  if (end == v || n < 0) {
    return -1;
  }
  return n;
}

int lsp_reader_next(lsp_reader *r, char **body_io) {
  size_t i = 0;
  size_t hdr_end = 0;
  size_t body_start = 0;
  long content_length = -1;
  *body_io = NULL;
  // Find the header terminator.
  for (i = r->pos; i + 3 < r->len; i++) {
    if (r->buf[i] == '\r' && r->buf[i + 1] == '\n' &&
        r->buf[i + 2] == '\r' && r->buf[i + 3] == '\n') {
      hdr_end = i;
      body_start = i + 4;
      break;
    }
  }
  if (body_start == 0) {
    for (i = r->pos; i + 1 < r->len; i++) {
      if (r->buf[i] == '\n' && r->buf[i + 1] == '\n') {
        hdr_end = i;
        body_start = i + 2;
        break;
      }
    }
  }
  if (body_start == 0) {
    return 0;
  }
  // Parse header lines for Content-Length (case-insensitive).
  {
    size_t p = r->pos;
    while (p < hdr_end) {
      size_t line_end = p;
      long n = 0;
      while (line_end < hdr_end && r->buf[line_end] != '\n') {
        line_end++;
      }
      n = lsp_header_length(r->buf, p, line_end);
      if (n >= 0) {
        content_length = n;
      }
      p = line_end + 1;
    }
  }
  if (content_length < 0) {
    // No usable length: skip this block and resync on the next.
    r->pos = body_start;
    return 2;
  }
  if (r->len - body_start < (size_t)content_length) {
    return 0;
  }
  *body_io = ccb_xmalloc((size_t)content_length + 1);
  memcpy(*body_io, r->buf + body_start, (size_t)content_length);
  (*body_io)[content_length] = '\0';
  r->pos = body_start + (size_t)content_length;
  return 1;
}

static int lsp_write_all(int fd, const char *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
  return 0;
}

static void lsp_send_raw(int out_fd, const char *json, size_t len) {
  ccb_str frame;
  ccb_str_init(&frame);
  ccb_str_appendf(&frame, "Content-Length: %zu\r\n\r\n", len);
  if (lsp_write_all(out_fd, frame.buf != NULL ? frame.buf : "",
      strlen(frame.buf != NULL ? frame.buf : "")) != 0) {
    ccb_str_free(&frame);
    return;
  }
  ccb_str_free(&frame);
  (void)lsp_write_all(out_fd, json, len);
}

static void lsp_send_json(int out_fd, const ccb_str *json) {
  lsp_send_raw(out_fd, json->buf != NULL ? json->buf : "",
    strlen(json->buf != NULL ? json->buf : ""));
}

// Serialize a request id verbatim: integral numbers as integers
// (ids are small), other numbers as-is, strings escaped, anything
// else as null.
static void lsp_write_id(ccb_str *out, const ccb_json_value *id) {
  if (id == NULL) {
    ccb_str_append(out, "null");
    return;
  }
  if (id->type == CCB_JSON_NUM) {
    double d = id->num;
    if (d == (double)(long long)d) {
      ccb_str_appendf(out, "%lld", (long long)d);
    } else {
      ccb_str_appendf(out, "%.17g", d);
    }
    return;
  }
  if (id->type == CCB_JSON_STR && id->str != NULL) {
    ccb_json_escape(out, id->str);
    return;
  }
  ccb_str_append(out, "null");
}

void lsp_send_result(int out_fd, const ccb_json_value *id,
    const char *result_json) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
  lsp_write_id(&out, id);
  ccb_str_append(&out, ",\"result\":");
  ccb_str_append(&out, result_json != NULL ? result_json : "null");
  ccb_str_append(&out, "}");
  lsp_send_json(out_fd, &out);
  ccb_str_free(&out);
}

void lsp_send_error(int out_fd, const ccb_json_value *id,
    long code, const char *message) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"jsonrpc\":\"2.0\",\"id\":");
  lsp_write_id(&out, id);
  ccb_str_appendf(&out, ",\"error\":{\"code\":%ld,\"message\":", code);
  ccb_json_escape(&out, message != NULL ? message : "error");
  ccb_str_append(&out, "}}");
  lsp_send_json(out_fd, &out);
  ccb_str_free(&out);
}

// Publish the current tree when it differs from the last publish
// (the first publish always sends: last_published starts NULL).
void lsp_maybe_publish(pv_lsp_server *s, int out_fd) {
  ccb_str out;
  const char *tree = NULL;
  if (s == NULL) {
    return;
  }
  tree = pv_lsp_tree_json(s);
  if (tree == NULL) {
    return;
  }
  if (s->last_published != NULL && strcmp(s->last_published, tree) == 0) {
    return;
  }
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"jsonrpc\":\"2.0\",\"method\":\"ccb/publishTree\",\"params\":");
  ccb_str_append(&out, tree);
  ccb_str_append(&out, "}");
  lsp_send_json(out_fd, &out);
  ccb_str_free(&out);
  free(s->last_published);
  s->last_published = ccb_xstrdup(tree);
}

// ---------- method helpers ----------

// file:// URI to path, with %XX decoding. Returns a malloc'd path or
// NULL for a non-file URI.
char *lsp_uri_to_path(const char *uri) {
  const char *p = NULL;
  ccb_str out;
  if (uri == NULL || strncmp(uri, "file://", 7) != 0) {
    return NULL;
  }
  p = uri + 7;
  if (strncmp(p, "localhost/", 10) == 0) {
    p += 9; // keep the leading slash of the path
  }
  ccb_str_init(&out);
  while (*p != '\0') {
    if (*p == '%' && p[1] != '\0' && p[2] != '\0' &&
        ((p[1] >= '0' && p[1] <= '9') ||
         (p[1] >= 'a' && p[1] <= 'f') ||
         (p[1] >= 'A' && p[1] <= 'F')) &&
        ((p[2] >= '0' && p[2] <= '9') ||
         (p[2] >= 'a' && p[2] <= 'f') ||
         (p[2] >= 'A' && p[2] <= 'F'))) {
      int hi = (p[1] <= '9') ? p[1] - '0' :
        ((p[1] <= 'F' && p[1] >= 'A') ? p[1] - 'A' + 10 : p[1] - 'a' + 10);
      int lo = (p[2] <= '9') ? p[2] - '0' :
        ((p[2] <= 'F' && p[2] >= 'A') ? p[2] - 'A' + 10 : p[2] - 'a' + 10);
      char c = (char)(hi * 16 + lo);
      ccb_str_append_n(&out, &c, 1);
      p += 3;
    } else {
      ccb_str_append_n(&out, p, 1);
      p++;
    }
  }
  if (out.buf == NULL) {
    return ccb_xstrdup("");
  }
  return out.buf;
}

// Re-root the server on a new workspace directory when it holds a
// usable ccb.toml (validated by a throwaway load first, so a bad
// root never disturbs the current project). Drops staging, docs,
// and extra stats; the next rebuild reloads the new config.
void lsp_reroot(pv_lsp_server *s, const char *dir) {
  char *toml = NULL;
  ccb_config *probe = NULL;
  ccb_diag_list dl;
  size_t i = 0;
  size_t dlen = 0;
  size_t clen = 0;
  if (s == NULL || dir == NULL) {
    return;
  }
  // Same directory under a different spelling (notably a trailing
  // slash from the client) is not a re-root: comparing raw strings
  // would drop every cache and, worse, suppress the initial publish
  // (the handler skips it when a rebuild is queued, and the rebuild
  // then reports the identical tree as unchanged).
  dlen = strlen(dir);
  while (dlen > 1 && dir[dlen - 1] == '/') {
    dlen--;
  }
  clen = strlen(s->config_dir);
  while (clen > 1 && s->config_dir[clen - 1] == '/') {
    clen--;
  }
  if (dlen == clen && memcmp(dir, s->config_dir, dlen) == 0) {
    return;
  }
  toml = ccb_path_join(dir, "ccb.toml");
  if (!ccb_path_exists(toml)) {
    free(toml);
    return;
  }
  ccb_diag_list_init(&dl);
  if (ccb_config_load(toml, NULL, &probe, &dl) != CCB_OK ||
      probe == NULL || !probe->has_preview ||
      (probe->preview_styles == NULL && probe->preview_flow == NULL)) {
    ccb_diag_list_free(&dl);
    if (probe != NULL) {
      ccb_config_free(probe);
    }
    free(toml);
    return;
  }
  ccb_diag_list_free(&dl);
  ccb_config_free(probe);
  // Commit: drop everything tied to the old root.
  for (i = 0; i < s->nflow; i++) {
    lsp_flow_entry_free(&s->flow[i]);
  }
  free(s->flow);
  s->flow = NULL;
  s->nflow = 0;
  s->capflow = 0;
  for (i = 0; i < s->ncss; i++) {
    lsp_css_entry_free(&s->css[i]);
  }
  free(s->css);
  s->css = NULL;
  s->ncss = 0;
  s->capcss = 0;
  for (i = 0; i < s->ndocs; i++) {
    free(s->docs[i].path);
    free(s->docs[i].text);
  }
  free(s->docs);
  s->docs = NULL;
  s->ndocs = 0;
  s->capdocs = 0;
  for (i = 0; i < s->nextra; i++) {
    free(s->extra[i].path);
  }
  free(s->extra);
  s->extra = NULL;
  s->nextra = 0;
  s->capextra = 0;
  free(s->toml_path);
  free(s->config_dir);
  s->toml_path = toml;
  s->config_dir = ccb_dirname(toml);
  s->config_dirty = 1;
  s->editor_dirty = 1;
}

// Pick the initialize root directory: rootUri wins, then the first
// workspace folder's uri, then rootPath. Returns a malloc'd path or
// NULL when none was given or usable.
// A non-empty path for a file:// uri string value, or NULL.
static char *lsp_uri_path(const char *uri) {
  char *p = lsp_uri_to_path(uri);
  if (p != NULL && p[0] != '\0') {
    return p;
  }
  free(p);
  return NULL;
}

// The first workspace folder's uri string, or NULL.
static const char *lsp_first_folder_uri(const ccb_json_value *params) {
  const ccb_json_value *folders = ccb_json_obj_get(params,
    "workspaceFolders");
  const ccb_json_value *f0 = NULL;
  const ccb_json_value *v = NULL;
  if (folders == NULL || folders->type != CCB_JSON_ARR ||
      folders->count == 0) {
    return NULL;
  }
  f0 = folders->items[0];
  if (f0 == NULL || f0->type != CCB_JSON_OBJ) {
    return NULL;
  }
  v = ccb_json_obj_get(f0, "uri");
  if (v == NULL || v->type != CCB_JSON_STR || v->str == NULL) {
    return NULL;
  }
  return v->str;
}

char *lsp_init_root(const ccb_json_value *params) {
  const ccb_json_value *v = NULL;
  const char *folder_uri = NULL;
  char *p = NULL;
  if (params == NULL || params->type != CCB_JSON_OBJ) {
    return NULL;
  }
  v = ccb_json_obj_get(params, "rootUri");
  if (v != NULL && v->type == CCB_JSON_STR && v->str != NULL) {
    p = lsp_uri_path(v->str);
    if (p != NULL) {
      return p;
    }
  }
  folder_uri = lsp_first_folder_uri(params);
  if (folder_uri != NULL) {
    p = lsp_uri_path(folder_uri);
    if (p != NULL) {
      return p;
    }
  }
  v = ccb_json_obj_get(params, "rootPath");
  if (v != NULL && v->type == CCB_JSON_STR && v->str != NULL &&
      v->str[0] != '\0') {
    return ccb_xstrdup(v->str);
  }
  return NULL;
}

// Diagnostics array JSON, mirroring pv_project_json's diagnostics
// section exactly (code/phase/message default "", file null when
// absent, line as-is).
void lsp_json_diagnostics(ccb_str *out, pv_project *p) {
  size_t i = 0;
  ccb_str_append(out, "{\"diagnostics\":[");
  if (p != NULL) {
    for (i = 0; i < p->diags.count; i++) {
      ccb_diag *d = &p->diags.items[i];
      if (i > 0) {
        ccb_str_append(out, ",");
      }
      ccb_str_append(out, "{\"code\":");
      if (d->code != NULL) {
        ccb_json_escape(out, d->code);
      } else {
        ccb_str_append(out, "\"\"");
      }
      ccb_str_append(out, ",\"phase\":");
      if (d->phase != NULL) {
        ccb_json_escape(out, d->phase);
      } else {
        ccb_str_append(out, "\"\"");
      }
      ccb_str_append(out, ",\"message\":");
      if (d->message != NULL) {
        ccb_json_escape(out, d->message);
      } else {
        ccb_str_append(out, "\"\"");
      }
      ccb_str_append(out, ",\"file\":");
      if (d->file != NULL) {
        ccb_json_escape(out, d->file);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_appendf(out, ",\"line\":%ld}", d->line);
    }
  }
  ccb_str_append(out, "]}");
}

// Incoming edges for a screen: every edge in every screen whose
// target is this screen's name, including self-edges.
// Append the edge's ,"args":[...] array (null for NULL args).
static void lsp_json_args(ccb_str *out, pv_edge *e) {
  size_t k = 0;
  ccb_str_append(out, ",\"args\":[");
  for (k = 0; k < e->nargs; k++) {
    if (k > 0) {
      ccb_str_append(out, ",");
    }
    if (e->args[k] != NULL) {
      ccb_json_escape(out, e->args[k]);
    } else {
      ccb_str_append(out, "null");
    }
  }
  ccb_str_append(out, "]");
}

void lsp_json_incoming(ccb_str *out, pv_project *p,
    const char *name) {
  size_t si = 0;
  size_t ei = 0;
  int first = 1;
  ccb_str_append(out, "[");
  if (p == NULL || name == NULL) {
    ccb_str_append(out, "]");
    return;
  }
  for (si = 0; si < p->nscreens; si++) {
    pv_screen *screen = p->screens[si];
    for (ei = 0; ei < screen->nedges; ei++) {
      pv_edge *e = &screen->edges[ei];
      if (e->target == NULL || strcmp(e->target, name) != 0) {
        continue;
      }
      if (!first) {
        ccb_str_append(out, ",");
      }
      first = 0;
      ccb_str_append(out, "{\"from_screen\":");
      if (screen->name != NULL) {
        ccb_json_escape(out, screen->name);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_append(out, ",\"from\":");
      if (e->from != NULL) {
        ccb_json_escape(out, e->from);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_append(out, ",\"target\":");
      ccb_json_escape(out, e->target);
      ccb_str_appendf(out, ",\"unknown\":%s", e->unknown ? "true" : "false");
      lsp_json_args(out, e);
      ccb_str_append(out, ",\"branch\":");
      if (e->branch != NULL) {
        ccb_json_escape(out, e->branch);
      } else {
        ccb_str_append(out, "null");
      }
      ccb_str_append(out, "}");
    }
  }
  ccb_str_append(out, "]");
}
