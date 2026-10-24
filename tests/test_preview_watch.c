// Tests for ccb_preview_watch: debounce (fake clock, no sleeping),
// watched-set snapshot diffs in a scratch dir, and the wait context
// (inotify when available, polling rescan otherwise). The snapshot is
// opaque by contract, so the watched set is verified behaviorally:
// a file the set should ignore must not flip changed(), and a file it
// should watch must. No test sleeps on a real loop: waits use bounded
// timeouts and the change is always made before the wait call.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_preview_watch.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void write_text(const char *path, const char *text) {
  ccb_status st = ccb_write_file(path, text, strlen(text));
  CHECK(st == CCB_OK);
}

static pv_watch_snapshot *take_ok(const char *cfg, const char *styles,
    const char *flow, const char *toml) {
  pv_watch_snapshot *snap = NULL;
  ccb_status st = pv_watch_snapshot_take(cfg, styles, flow, toml, &snap);
  CHECK(st == CCB_OK);
  CHECK(snap != NULL);
  return snap;
}

int main(void) {
  char tmpl[] = "/tmp/ccb-watch-test-XXXXXX";
  char *root = mkdtemp(tmpl);
  char *styles = NULL;
  char *flow = NULL;
  char *sub = NULL;
  char *toml = NULL;
  char *p = NULL;
  pv_watch_snapshot *snap_rel = NULL;
  pv_watch_snapshot *snap_abs = NULL;
  pv_watch_snapshot *snap = NULL;
  pv_watch_snapshot *next = NULL;
  pv_watch_debounce d;
  pv_watch_ctx *ctx = NULL;
  int used_inotify = -1;
  ccb_status st;
  long long t1 = 0;
  long long t2 = 0;

  CHECK(root != NULL);
  if (root == NULL) {
    TEST_MAIN_END();
  }
  styles = ccb_path_join(root, "styles");
  flow = ccb_path_join(root, "flow");
  sub = ccb_path_join(styles, "sub");
  toml = ccb_path_join(root, "ccb.toml");
  CHECK(ccb_mkdir_p(styles) == CCB_OK);
  CHECK(ccb_mkdir_p(sub) == CCB_OK);
  CHECK(ccb_mkdir_p(flow) == CCB_OK);

  p = ccb_path_join(styles, "a.css");
  write_text(p, "text { color: #111111; }");
  free(p);
  p = ccb_path_join(styles, "b.txt");
  write_text(p, "not a stylesheet");
  free(p);
  p = ccb_path_join(sub, "c.css");
  write_text(p, "button { margin: 8px; }");
  free(p);
  p = ccb_path_join(flow, "Main.kt");
  write_text(p, "fun Home() {}");
  free(p);
  p = ccb_path_join(flow, "notes.txt");
  write_text(p, "flow notes");
  free(p);
  write_text(toml, "[preview]\n");

  // ---------- debounce: fake clock, no sleeping ----------
  memset(&d, 0, sizeof(d));
  CHECK(pv_watch_debounce_due(&d, 1000) == 0); // nothing pending
  pv_watch_debounce_event(&d, 1000);
  CHECK(pv_watch_debounce_due(&d, 1000) == 0);
  CHECK(pv_watch_debounce_due(&d, 1199) == 0);
  CHECK(pv_watch_debounce_due(&d, 1200) == 1); // exactly 200ms: due
  // Burst coalescing: a late event postpones the due time.
  pv_watch_debounce_event(&d, 1300);
  CHECK(pv_watch_debounce_due(&d, 1499) == 0);
  CHECK(pv_watch_debounce_due(&d, 1500) == 1);
  pv_watch_debounce_reset(&d);
  CHECK(pv_watch_debounce_due(&d, 100000) == 0);
  pv_watch_debounce_event(&d, 5000);
  pv_watch_debounce_event(&d, 5100); // burst: second event wins
  CHECK(pv_watch_debounce_due(&d, 5299) == 0);
  CHECK(pv_watch_debounce_due(&d, 5300) == 1);
  pv_watch_debounce_reset(&d);

  // Monotonic clock: non-decreasing, millisecond scale.
  t1 = pv_watch_now_ms();
  t2 = pv_watch_now_ms();
  CHECK(t2 >= t1);
  CHECK(t1 > 0);

  // ---------- watched set: relative vs absolute resolution ----------
  // Relative styles/flow/toml resolve against config_dir (the toml's
  // directory), so the relative snapshot must equal the absolute one.
  snap_rel = take_ok(root, "styles", "flow", "ccb.toml");
  snap_abs = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap_rel, snap_abs) == 0);
  CHECK(pv_watch_snapshot_changed(snap_abs, snap_rel) == 0);
  pv_watch_snapshot_free(snap_rel);
  pv_watch_snapshot_free(snap_abs);
  snap_rel = NULL;
  snap_abs = NULL;

  // ---------- watched-set changes ----------
  snap = take_ok(root, styles, flow, toml);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 0);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // New *.css in styles: watched.
  p = ccb_path_join(styles, "d.css");
  write_text(p, "screen { padding: 16px; }");
  free(p);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 1);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // Non-css file in styles: NOT watched (mirrors pv_project_load's
  // ".css" filter), so the diff must stay quiet.
  p = ccb_path_join(styles, "e.txt");
  write_text(p, "ignored");
  free(p);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 0);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // Any file in flow: watched (the set is wider than the *.kt parse).
  p = ccb_path_join(flow, "readme.md");
  write_text(p, "flow readme");
  free(p);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 1);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // Nested *.css (recursive enumeration): watched.
  p = ccb_path_join(sub, "deep.css");
  write_text(p, "row { gap: 4px; }");
  free(p);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 1);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // Content change with a size change: watched.
  p = ccb_path_join(styles, "a.css");
  write_text(p, "text { color: #111111; font-size: 16px; } longer");
  free(p);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 1);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // Documented blind spot: rewrite with identical size AND the mtime
  // restored to its previous value. The snapshot diff cannot see this
  // (no content hashing), and that is acceptable: the render loop
  // re-reads from disk whenever the diff does fire.
  p = ccb_path_join(styles, "a.css");
  {
    struct stat before;
    struct timespec times[2];
    CHECK(stat(p, &before) == 0);
    write_text(p, "text { color: #222222; font-size: 16px; } longer");
    times[0] = before.st_atim;
    times[1] = before.st_mtim;
    CHECK(utimensat(AT_FDCWD, p, times, 0) == 0);
  }
  free(p);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 0);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // Delete a flow file: watched.
  p = ccb_path_join(flow, "notes.txt");
  CHECK(ccb_remove_recursive(p) == CCB_OK);
  free(p);
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 1);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // ccb.toml itself is watched: modifying it flips the diff.
  write_text(toml, "[preview]\nstrict = true\n");
  next = take_ok(root, styles, flow, toml);
  CHECK(pv_watch_snapshot_changed(snap, next) == 1);
  pv_watch_snapshot_free(snap);
  snap = next;
  next = NULL;

  // ---------- missing dirs / NULL sections are not errors ----------
  {
    pv_watch_snapshot *miss = take_ok(root, "nope-styles", "nope-flow",
      toml);
    pv_watch_snapshot *toml_only = take_ok(root, NULL, NULL, toml);
    CHECK(pv_watch_snapshot_changed(miss, toml_only) == 0);
    pv_watch_snapshot_free(miss);
    pv_watch_snapshot_free(toml_only);
  }
  st = pv_watch_snapshot_take(root, styles, flow, toml, NULL);
  CHECK(st != CCB_OK);
  CHECK(pv_watch_snapshot_changed(NULL, NULL) == 0);
  CHECK(pv_watch_snapshot_changed(NULL, snap) == 1);
  CHECK(pv_watch_snapshot_changed(snap, NULL) == 1);
  pv_watch_snapshot_free(snap);
  snap = NULL;
  pv_watch_snapshot_free(NULL); // NULL-safe

  // ---------- wait context: rescan is the source of truth ----------
  // The change is always made BEFORE the wait, so no test sleeps on a
  // real loop: the inotify backend wakes on the queued event and the
  // polling backend sees the change on its first rescan. The
  // snapshot-diff assertions above exercise the rescan path directly,
  // which is the same code pv_watch_wait uses for polling.
  st = pv_watch_begin(root, "styles", "flow", "ccb.toml", &ctx,
    &used_inotify);
  CHECK(st == CCB_OK);
  CHECK(ctx != NULL);
  CHECK(used_inotify == 0 || used_inotify == 1);
  printf("watch backend: %s\n", used_inotify ? "inotify" : "polling");
  CHECK(pv_watch_wait(NULL, 100) == -1);
  CHECK(pv_watch_wait(ctx, 100) == 0); // quiet: timeout, no change
  p = ccb_path_join(flow, "Main.kt");
  write_text(p, "fun Home() {}\nfun Detail() {}");
  free(p);
  CHECK(pv_watch_wait(ctx, 2000) == 1); // modify detected
  CHECK(pv_watch_wait(ctx, 100) == 0); // snapshot updated: quiet again
  p = ccb_path_join(styles, "new.css");
  write_text(p, "box { margin: 2px; }");
  free(p);
  CHECK(pv_watch_wait(ctx, 2000) == 1); // create detected
  p = ccb_path_join(styles, "new.css");
  CHECK(ccb_remove_recursive(p) == CCB_OK);
  free(p);
  CHECK(pv_watch_wait(ctx, 2000) == 1); // delete detected
  CHECK(pv_watch_wait(ctx, -5) == 0); // negative timeout: one check
  pv_watch_end(ctx);
  ctx = NULL;
  pv_watch_end(NULL); // NULL-safe

  // ---------- signal flag: handlers only set the flag ----------
  CHECK(pv_watch_stop == 0);
  pv_watch_install_signals();
  CHECK(raise(SIGINT) == 0);
  CHECK(pv_watch_stop == 1);
  pv_watch_stop = 0;

  free(styles);
  free(flow);
  free(sub);
  free(toml);
  CHECK(ccb_remove_recursive(root) == CCB_OK);
  TEST_MAIN_END();
}
