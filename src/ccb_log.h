// ccb_log: quiet-by-default logging. In JSON mode, phase/warning/cache
// events go to stdout as JSONL; in human mode they go to stderr as short
// lines. Human prose never appears on stdout in JSON mode.
#ifndef CCB_LOG_H
#define CCB_LOG_H

#include "ccb_common.h"

typedef struct {
  int json;
  int verbose;
  int explain;
  // Reason of the last cache event logged, borrowed (a string
  // literal at the call site). The graph executor reads it after
  // a skipped task to record the task's skip_reason, and clears it
  // before each task so a reason never leaks across tasks.
  const char *last_cache_reason;
} ccb_log;

void ccb_log_init(ccb_log *log, int json, int verbose);
void ccb_log_phase_start(ccb_log *log, const char *phase);
void ccb_log_phase_done(ccb_log *log, const char *phase, long duration_ms);
void ccb_log_warning(ccb_log *log, const char *fmt, ...);
void ccb_log_info(ccb_log *log, const char *fmt, ...);
void ccb_log_cache(ccb_log *log, const char *stage, int hit, const char *reason);
// Task lifecycle events (Phase 1 graph execution). In JSON mode they
// go to stdout as {"event":"task","task":"<id>",...} JSONL; in human
// mode they are silent — the per-phase lines already show progress.
void ccb_log_task_start(ccb_log *log, const char *task_id);
void ccb_log_task_done(ccb_log *log, const char *task_id,
  long duration_ms, int skipped, const char *skip_reason);
void ccb_log_task_failed(ccb_log *log, const char *task_id,
  long duration_ms);
// Pass tool output through in verbose mode, delimited per stage.
void ccb_log_tool_output(ccb_log *log, const char *phase, const char *out, const char *err);

#endif
