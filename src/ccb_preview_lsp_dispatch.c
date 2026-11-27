// ccb_preview_lsp_dispatch: message dispatch + run loop.
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

// ---------- message dispatch ----------

// Handle one parsed message body. Returns 1 when the loop should
// exit (exit notification), 0 otherwise. Never rebuilds: handlers
// only update state; the loop rebuilds when editor_dirty is set or
// the disk debounce fires, so one rebuild covers a burst.
// The document uri of a didOpen/didChange/didClose notification's
// params: the textDocument.uri string, or NULL. The textDocument
// object is returned in *doc_io (NULL when absent).
static const char *lsp_doc_uri(const ccb_json_value *params,
    const ccb_json_value **doc_io) {
  const ccb_json_value *doc = NULL;
  const ccb_json_value *uri_v = NULL;
  if (params == NULL || params->type != CCB_JSON_OBJ) {
    return NULL;
  }
  doc = ccb_json_obj_get(params, "textDocument");
  if (doc == NULL || doc->type != CCB_JSON_OBJ) {
    return NULL;
  }
  *doc_io = doc;
  uri_v = ccb_json_obj_get(doc, "uri");
  if (uri_v == NULL || uri_v->type != CCB_JSON_STR) {
    return NULL;
  }
  return uri_v->str;
}

// The new document text of a didOpen/didChange notification:
// didOpen carries it on the document; didChange (full sync) on the
// last content change. NULL when there is none.
static const ccb_json_value *lsp_doc_text(const char *method,
    const ccb_json_value *params, const ccb_json_value *doc) {
  const ccb_json_value *changes = NULL;
  const ccb_json_value *last = NULL;
  if (strcmp(method, "textDocument/didOpen") == 0) {
    // didOpen carries the text on the document itself.
    if (doc == NULL) {
      return NULL;
    }
    return ccb_json_obj_get(doc, "text");
  }
  if (params == NULL || params->type != CCB_JSON_OBJ) {
    return NULL;
  }
  changes = ccb_json_obj_get(params, "contentChanges");
  if (changes == NULL || changes->type != CCB_JSON_ARR ||
      changes->count == 0) {
    return NULL;
  }
  // Full sync: the last change carries the whole text.
  last = changes->items[changes->count - 1];
  if (last == NULL || last->type != CCB_JSON_OBJ) {
    return NULL;
  }
  return ccb_json_obj_get(last, "text");
}

// Handle a textDocument/didOpen, didChange, or didClose
// notification: sync the document text (or drop it on close).
static void lsp_handle_doc(pv_lsp_server *s, const char *method,
    const ccb_json_value *params, int is_close) {
  const ccb_json_value *doc = NULL;
  const char *uri = lsp_doc_uri(params, &doc);
  const ccb_json_value *text_v = NULL;
  char *path = NULL;
  if (!is_close) {
    text_v = lsp_doc_text(method, params, doc);
  }
  if (uri == NULL) {
    return;
  }
  path = lsp_uri_to_path(uri);
  if (path == NULL) {
    return;
  }
  if (is_close) {
    pv_lsp_did_close(s, path);
  } else if (text_v != NULL && text_v->type == CCB_JSON_STR &&
      text_v->str != NULL) {
    if (strcmp(method, "textDocument/didOpen") == 0) {
      pv_lsp_did_open(s, path, text_v->str);
    } else {
      pv_lsp_did_change(s, path, text_v->str);
    }
  }
  free(path);
}

// The screen named `name` in project p, or NULL.
static pv_screen *lsp_find_screen(pv_project *p, const char *name) {
  size_t i = 0;
  if (p == NULL) {
    return NULL;
  }
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
}

static int lsp_handle(pv_lsp_server *s, const char *body, int out_fd) {
  ccb_json_value *msg = NULL;
  char *err = NULL;
  const ccb_json_value *method_v = NULL;
  const ccb_json_value *id_v = NULL;
  const ccb_json_value *params = NULL;
  const char *method = NULL;
  int is_request = 0;
  int rc = 0;
  msg = ccb_json_parse((ccb_slice){ body, strlen(body) }, &err);
  if (msg == NULL) {
    free(err);
    lsp_send_error(out_fd, NULL, -32700, "Parse error");
    return 0;
  }
  if (msg->type != CCB_JSON_OBJ) {
    ccb_json_free(msg);
    return 0;
  }
  method_v = ccb_json_obj_get(msg, "method");
  if (method_v == NULL || method_v->type != CCB_JSON_STR ||
      method_v->str == NULL) {
    // A client response (or a malformed message): the server never
    // sends requests, so there is nothing to answer.
    ccb_json_free(msg);
    return 0;
  }
  method = method_v->str;
  id_v = ccb_json_obj_get(msg, "id");
  is_request = id_v != NULL;
  params = ccb_json_obj_get(msg, "params");
  if (s->shutdown && is_request) {
    lsp_send_error(out_fd, id_v, -32600, "server shut down");
    ccb_json_free(msg);
    return 0;
  }
  if (s->shutdown && !is_request) {
    // Notifications after shutdown are ignored, except exit.
    if (strcmp(method, "exit") == 0) {
      ccb_json_free(msg);
      return 1;
    }
    ccb_json_free(msg);
    return 0;
  }
  if (!is_request) {
    // Notifications.
    if (strcmp(method, "exit") == 0) {
      ccb_json_free(msg);
      return 1;
    }
    if (strcmp(method, "textDocument/didOpen") == 0 ||
        strcmp(method, "textDocument/didChange") == 0 ||
        strcmp(method, "textDocument/didClose") == 0) {
      lsp_handle_doc(s, method, params,
        strcmp(method, "textDocument/didClose") == 0);
      ccb_json_free(msg);
      return 0;
    }
    // initialized, didSave, $/cancelRequest, and anything else:
    // no action (didSave needs none: the text already synced).
    ccb_json_free(msg);
    return 0;
  }
  // Requests.
  if (strcmp(method, "initialize") == 0) {
    char *root = lsp_init_root(params);
    ccb_str result;
    if (root != NULL) {
      lsp_reroot(s, root);
      free(root);
    }
    ccb_str_init(&result);
    ccb_str_append(&result, "{\"capabilities\":{\"textDocumentSync\":1},");
    ccb_str_append(&result, "\"serverInfo\":{\"name\":\"ccb lsp\",\"version\":\"");
    ccb_str_append(&result, CCB_VERSION);
    ccb_str_append(&result, "\"}}");
    lsp_send_result(out_fd, id_v, result.buf != NULL ? result.buf : "{}");
    ccb_str_free(&result);
    // First publish, unless a re-root queued a rebuild that will
    // publish the new tree right after.
    if (!s->editor_dirty) {
      lsp_maybe_publish(s, out_fd);
    }
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "shutdown") == 0) {
    s->shutdown = 1;
    lsp_send_result(out_fd, id_v, "null");
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "ccb/logicTree") == 0) {
    const char *tree = pv_lsp_tree_json(s);
    if (tree == NULL) {
      lsp_send_error(out_fd, id_v, -32603, "no project loaded");
    } else {
      lsp_send_result(out_fd, id_v, tree);
    }
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "ccb/screenInfo") == 0) {
    const ccb_json_value *name_v = NULL;
    pv_project *p = pv_lsp_project(s);
    pv_screen *screen = NULL;
    if (params != NULL && params->type == CCB_JSON_OBJ) {
      name_v = ccb_json_obj_get(params, "screen");
    }
    if (name_v != NULL && name_v->type == CCB_JSON_STR &&
        name_v->str != NULL) {
      screen = lsp_find_screen(p, name_v->str);
    }
    if (screen == NULL) {
      lsp_send_error(out_fd, id_v, -32602, "unknown screen");
    } else {
      ccb_str result;
      ccb_str_init(&result);
      ccb_str_append(&result, "{\"screen\":");
      pv_project_json_screen(&result, p, screen);
      ccb_str_append(&result, ",\"incoming\":");
      lsp_json_incoming(&result, p, screen->name);
      ccb_str_append(&result, "}");
      lsp_send_result(out_fd, id_v,
        result.buf != NULL ? result.buf : "{}");
      ccb_str_free(&result);
    }
    ccb_json_free(msg);
    return 0;
  }
  if (strcmp(method, "ccb/diagnostics") == 0) {
    ccb_str result;
    ccb_str_init(&result);
    lsp_json_diagnostics(&result, pv_lsp_project(s));
    lsp_send_result(out_fd, id_v,
      result.buf != NULL ? result.buf : "{}");
    ccb_str_free(&result);
    ccb_json_free(msg);
    return 0;
  }
  lsp_send_error(out_fd, id_v, -32601, "Method not found");
  ccb_json_free(msg);
  return rc;
}

// ---------- run loop ----------

// Drain stdin messages (non-blocking reads). Returns 1 when the
// exit notification was handled.
static int lsp_drain(pv_lsp_server *s, lsp_reader *rd, int out_fd) {
  for (;;) {
    char *body = NULL;
    int pr = lsp_reader_next(rd, &body);
    if (pr == 0) {
      if (!lsp_reader_more(rd)) {
        break;
      }
      continue;
    }
    if (pr == 2) {
      continue; // skipped a header block without a length
    }
    if (lsp_handle(s, body, out_fd)) {
      free(body);
      return 1;
    }
    free(body);
  }
  return 0;
}

int pv_lsp_run(pv_lsp_server *s, int in_fd, int out_fd) {
  lsp_reader rd;
  int exit_requested = 0;
  int flags = 0;
  memset(&rd, 0, sizeof(rd));
  rd.fd = in_fd;
  if (s == NULL) {
    return 1;
  }
  flags = fcntl(in_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(in_fd, F_SETFL, flags | O_NONBLOCK);
  }
  pv_watch_install_signals();
  for (;;) {
    // 1. Drain stdin messages (non-blocking reads).
    exit_requested = lsp_drain(s, &rd, out_fd);
    if (exit_requested || rd.eof || pv_watch_stop) {
      break;
    }
    // 2. Editor updates rebuild immediately (a burst still costs
    // one rebuild per message; preview projects are small). Publish
    // when the tree changed, or when nothing was ever published:
    // a re-root that rebuilds to an identical tree (e.g. a root
    // spelling that differs from the discovered directory) must
    // not swallow the first publish.
    if (s->editor_dirty) {
      int rrc = pv_lsp_rebuild(s);
      if (rrc == 1 || s->last_published == NULL) {
        lsp_maybe_publish(s, out_fd);
      }
    }
    if (rd.eof || pv_watch_stop) {
      break;
    }
    // 3. Sleep on the inotify wakeup (bounded 50ms so disk changes
    // surface promptly even without an event), then scan + debounce.
    if (s->watch_ctx != NULL) {
      (void)pv_watch_wait(s->watch_ctx, 50);
    } else {
      poll(NULL, 0, 50);
    }
    if (pv_watch_stop) {
      break;
    }
    (void)pv_lsp_scan_disk(s);
    if (pv_watch_debounce_due(&s->deb, pv_watch_now_ms())) {
      pv_watch_debounce_reset(&s->deb);
      if (pv_lsp_rebuild(s) == 1) {
        lsp_maybe_publish(s, out_fd);
      }
    }
    if (rd.eof || pv_watch_stop) {
      break;
    }
  }
  free(rd.buf);
  return s->shutdown ? 0 : 1;
}
