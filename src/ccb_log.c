// ccb_log implementation.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_json.h"
#include "ccb_str.h"
#include "ccb_log.h"
#include <stdarg.h>

void ccb_log_init(ccb_log *log, int json, int verbose) {
  log->json = json;
  log->verbose = verbose;
  log->explain = 0;
  log->last_cache_reason = NULL;
}

void ccb_log_phase_start(ccb_log *log, const char *phase) {
  if (log->json) {
    ccb_str out;
    ccb_str_init(&out);
    ccb_str_append(&out, "{\"event\":\"phase\",\"phase\":");
    ccb_json_escape(&out, phase);
    ccb_str_append(&out, ",\"status\":\"started\"}\n");
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
  } else {
    fprintf(stderr, "ccb: %s...\n", phase);
  }
}

void ccb_log_phase_done(ccb_log *log, const char *phase, long duration_ms) {
  if (log->json) {
    ccb_str out;
    ccb_str_init(&out);
    ccb_str_append(&out, "{\"event\":\"phase\",\"phase\":");
    ccb_json_escape(&out, phase);
    ccb_str_appendf(&out, ",\"status\":\"finished\",\"duration_ms\":%ld}\n", duration_ms);
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
  } else {
    fprintf(stderr, "ccb: %s done (%ld ms)\n", phase, duration_ms);
  }
}

void ccb_log_warning(ccb_log *log, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (log->json) {
    ccb_str out;
    ccb_str_init(&out);
    ccb_str_append(&out, "{\"event\":\"warning\",\"message\":");
    ccb_json_escape(&out, buf);
    ccb_str_append(&out, "}\n");
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
  } else {
    fprintf(stderr, "ccb: warning: %s\n", buf);
  }
}

void ccb_log_info(ccb_log *log, const char *fmt, ...) {
  if (log->json) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "ccb: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

void ccb_log_cache(ccb_log *log, const char *stage, int hit, const char *reason) {
  // Record the reason for the executor's skip_reason, even in
  // explain mode (where the event itself is suppressed): the
  // reason is task-result data, not chatter.
  log->last_cache_reason = reason;
  // In explain mode the stdout step table is the product; cache chatter
  // on stderr would only be noise.
  if (log->explain) {
    return;
  }
  if (log->json) {
    ccb_str out;
    ccb_str_init(&out);
    ccb_str_append(&out, "{\"event\":\"cache\",\"stage\":");
    ccb_json_escape(&out, stage);
    ccb_str_append(&out, hit ? ",\"hit\":true" : ",\"hit\":false");
    ccb_str_append(&out, ",\"reason\":");
    ccb_json_escape(&out, reason);
    ccb_str_append(&out, "}\n");
    fputs(out.buf != NULL ? out.buf : "", stdout);
    ccb_str_free(&out);
  } else {
    fprintf(stderr, "ccb: cache %s: %s (%s)\n", stage, hit ? "hit" : "miss", reason);
  }
}

void ccb_log_task_start(ccb_log *log, const char *task_id) {
  if (!log->json) {
    return;
  }
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"event\":\"task\",\"task\":");
  ccb_json_escape(&out, task_id);
  ccb_str_append(&out, ",\"status\":\"started\"}\n");
  fputs(out.buf != NULL ? out.buf : "", stdout);
  ccb_str_free(&out);
}

void ccb_log_task_done(ccb_log *log, const char *task_id,
    long duration_ms, int skipped, const char *skip_reason) {
  if (!log->json) {
    return;
  }
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"event\":\"task\",\"task\":");
  ccb_json_escape(&out, task_id);
  ccb_str_appendf(&out, ",\"status\":\"finished\",\"duration_ms\":%ld,\"result\":\"%s\"",
    duration_ms, skipped ? "skipped" : "done");
  if (skip_reason != NULL) {
    ccb_str_append(&out, ",\"reason\":");
    ccb_json_escape(&out, skip_reason);
  }
  ccb_str_append(&out, "}\n");
  fputs(out.buf != NULL ? out.buf : "", stdout);
  ccb_str_free(&out);
}

void ccb_log_task_failed(ccb_log *log, const char *task_id,
    long duration_ms) {
  if (!log->json) {
    return;
  }
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"event\":\"task\",\"task\":");
  ccb_json_escape(&out, task_id);
  ccb_str_appendf(&out, ",\"status\":\"finished\",\"duration_ms\":%ld,\"result\":\"failed\"}\n",
    duration_ms);
  fputs(out.buf != NULL ? out.buf : "", stdout);
  ccb_str_free(&out);
}

void ccb_log_tool_output(ccb_log *log, const char *phase, const char *out, const char *err) {
  if (!log->verbose) {
    return;
  }
  if (log->json) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "{\"event\":\"tool_output\",\"phase\":");
    ccb_json_escape(&s, phase);
    ccb_str_append(&s, ",\"stdout\":");
    ccb_json_escape(&s, out == NULL ? "" : out);
    ccb_str_append(&s, ",\"stderr\":");
    ccb_json_escape(&s, err == NULL ? "" : err);
    ccb_str_append(&s, "}\n");
    fputs(s.buf, stdout);
    ccb_str_free(&s);
    return;
  }
  fprintf(stderr, "ccb: --- %s stdout ---\n%s", phase, out != NULL ? out : "");
  fprintf(stderr, "ccb: --- %s stderr ---\n%s", phase, err != NULL ? err : "");
}
