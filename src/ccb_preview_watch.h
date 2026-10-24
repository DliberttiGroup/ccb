// ccb_preview_watch: I/O-free watch core for `ccb preview --walk` watch
// mode (the render loop lives in the CLI worker and consumes this API).
//
// Watched set, mirroring pv_project_load's file enumeration exactly:
// every *.css file under the styles directory (recursive, sorted), every
// file under the flow directory (recursive, sorted, any extension —
// note pv_project_load parses only *.kt from the flow dir, but the watch
// set is deliberately wider: any flow-dir file may become a screen), and
// the ccb.toml file itself. Directory entries are never snapshot
// entries: a new or deleted file is detected because the rescan's file
// list differs from the previous snapshot's.
//
// Path resolution mirrors ccb_config: cfg->preview_styles and
// cfg->preview_flow are already absolute when they come from
// ccb_config_load (resolved against the directory containing ccb.toml).
// This module accepts either form: an absolute styles/flow/toml path is
// used as-is, a relative one is resolved against config_dir (the
// directory containing ccb.toml). Paths are never resolved against the
// process working directory blindly. When config_dir is NULL it is
// derived from toml_path's directory; when both are NULL, relative
// inputs are used as given.
//
// Missing styles/flow directories are NOT errors: they contribute
// nothing to the snapshot, matching preview's tolerant load (a project
// may declare only one of styles/flow). A snapshot is therefore always
// returned on success, possibly empty.
//
// Change detection is by (path, mtime seconds, mtime nanoseconds, size).
// Documented limitation: a file rewritten with byte-identical size and
// an mtime restored to its previous value (e.g. touch -r) is invisible
// to the snapshot diff, by design — content hashing is out of scope
// for a wakeup mechanism; the render loop re-renders from disk anyway
// whenever the diff fires.
//
// inotify is a wakeup, never the source of truth: pv_watch_wait rescans
// and diffs on every outcome (event, timeout, or poll slice), and the
// CLI loop re-renders from the rescan, not from the raw event stream.
// New files appearing later are picked up by that rescan even if their
// inotify file watch was never installed.
//
// Ownership: snapshots and contexts are malloc'd and owned by the
// caller; free them with pv_watch_snapshot_free / pv_watch_end. The
// debounce struct is a plain value owned by the caller. The signal flag
// is the one global mutable state in this module (the §9 exception the
// task allows): the handlers only set it, never do I/O or exit.
#ifndef CCB_PREVIEW_WATCH_H
#define CCB_PREVIEW_WATCH_H

#include "ccb_common.h"
#include <signal.h>

// Opaque snapshot: a sorted list of (path, mtime, size) entries for
// every watched file at take time. Malloc'd; free with
// pv_watch_snapshot_free. A NULL snapshot pointer is treated as an
// empty snapshot by pv_watch_snapshot_changed.
typedef struct pv_watch_snapshot pv_watch_snapshot;

// Take a snapshot of the watched set. styles_dir and flow_dir may be
// NULL (absent section); toml_path is the discovered ccb.toml path
// (may be NULL, in which case no toml entry is recorded). Returns
// CCB_OK with an (possibly empty) snapshot in *out; CCB_ERR_USAGE when
// out is NULL. Missing styles/flow directories and a missing toml file
// are not errors.
ccb_status pv_watch_snapshot_take(const char *config_dir,
  const char *styles_dir, const char *flow_dir, const char *toml_path,
  pv_watch_snapshot **out);
void pv_watch_snapshot_free(pv_watch_snapshot *snap);

// 1 when any path was added, removed, or changed (mtime seconds,
// mtime nanoseconds, or size) between old_snap and new_snap; 0 when
// the two snapshots describe identical watched sets. NULL counts as
// an empty snapshot.
int pv_watch_snapshot_changed(const pv_watch_snapshot *old_snap,
  const pv_watch_snapshot *new_snap);

// Debounce: pure, injectable clock, no sleeping, no I/O. An event
// starts a 200ms quiet window; a later event restarts it. The CLI loop
// calls pv_watch_debounce_event on every pv_watch_wait hit and
// re-renders only once pv_watch_debounce_due reports true.
typedef struct {
  long long last_event_ms;
  int pending;
} pv_watch_debounce;

// Record an event at now_ms and mark a render pending.
void pv_watch_debounce_event(pv_watch_debounce *d, long long now_ms);
// 1 iff an event is pending AND now_ms - last_event_ms >= 200.
int pv_watch_debounce_due(const pv_watch_debounce *d, long long now_ms);
// Clear the pending flag (call after re-rendering).
void pv_watch_debounce_reset(pv_watch_debounce *d);

// Monotonic clock in milliseconds (CLOCK_MONOTONIC). Never wall time:
// a wall-clock jump must not fire or suppress a debounce.
long long pv_watch_now_ms(void);

// Opaque watch context: the last snapshot plus, on Linux, the inotify
// fd and watches. Malloc'd; free with pv_watch_end.
typedef struct pv_watch_ctx pv_watch_ctx;

// Start watching. Tries inotify first: non-recursive watches on the
// styles dir and flow dir (when present) for create/delete/move, plus
// watches on each currently-watched file for modify/attrib/delete/move.
// When inotify_init fails (or the platform has no inotify), falls back
// to polling with no watches. *used_inotify_out is 1 for the inotify
// backend, 0 for polling. Returns CCB_OK on success (either backend);
// CCB_ERR_USAGE when out or used_inotify_out is NULL.
ccb_status pv_watch_begin(const char *config_dir, const char *styles_dir,
  const char *flow_dir, const char *toml_path, pv_watch_ctx **out,
  int *used_inotify_out);

// Block up to timeout_ms for a possible change. Returns 1 when a rescan
// differs from the context's last snapshot (the context snapshot is
// updated when it differs, and file watches are refreshed to cover new
// files); 0 on timeout with no change (including an EINTR interruption,
// so the caller can re-check pv_watch_stop); -1 on error (NULL context
// or a wait failure). inotify events are wakeups only: an event that
// produces no snapshot diff (IN_IGNORED queued by a watch refresh, an
// untracked file created in a watched dir, an attrib-only change) is
// consumed and waiting continues until the timeout — the snapshot diff
// is the source of truth, so a raw event alone never reports a change.
// Never blocks forever when timeout_ms >= 0; a negative timeout behaves
// like 0 (one immediate check, no blocking).
int pv_watch_wait(pv_watch_ctx *ctx, long timeout_ms);

// Close the inotify fd (when any) and free the context and its
// snapshot. NULL-safe.
void pv_watch_end(pv_watch_ctx *ctx);

// Signal flag the CLI loop checks: set by SIGINT/SIGTERM, cleared only
// by the loop itself (this module never clears it).
extern volatile sig_atomic_t pv_watch_stop;
// Install SIGINT/SIGTERM handlers that ONLY set pv_watch_stop (no I/O,
// no exit in the handler). Best effort: install failures are ignored,
// since the loop also exits on EOF/quit keys.
void pv_watch_install_signals(void);

#endif
