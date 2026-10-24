// ccb_preview_watch: I/O-free watch core for preview watch mode.
// Implements ccb_preview_watch.h. Snapshot/rescan logic is shared by
// both backends: the polling backend loops on it, and the inotify
// backend rescans on every event and every timeout so a missed or
// coalesced event can never hide a change the snapshot would show.
#include "ccb_common.h"
#include "ccb_preview_watch.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

#define PV_WATCH_DEBOUNCE_MS 200
#define PV_WATCH_POLL_SLICE_MS 50
// inotify masks: dirs watch for create/delete/move (plus the dir itself
// going away); files watch for content/attribute change and removal.
#define PV_WATCH_DIR_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | \
  IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF)
#define PV_WATCH_FILE_MASK (IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | \
  IN_MOVE_SELF)

// One snapshot entry: a watched file's identity for diffing.
typedef struct {
  char *path; // malloc'd, owned by the snapshot
  long long mtime_sec;
  long mtime_nsec;
  long long size;
} pv_watch_entry;

struct pv_watch_snapshot {
  pv_watch_entry *entries; // sorted by path (strcmp), deduplicated
  size_t count, cap;
};

struct pv_watch_ctx {
  char *styles_dir; // resolved (absolute when config_dir was absolute)
  char *flow_dir; // resolved, NULL when the section is absent
  char *toml_path; // resolved, NULL when absent
  pv_watch_snapshot *snap; // last snapshot; updated by pv_watch_wait
  int inotify_fd; // -1 for the polling backend
  int used_inotify;
  int *file_wds; // inotify watch descriptors, one per snapshot entry
  size_t nfile_wds, capfile_wds; // only meaningful on inotify
};

volatile sig_atomic_t pv_watch_stop = 0;

// Signal handler: set the flag and nothing else (no I/O, no exit).
static void pv_watch_on_signal(int sig) {
  CCB_UNUSED(sig);
  pv_watch_stop = 1;
}

void pv_watch_install_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = pv_watch_on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

// ---------- path resolution ----------

// Resolve one config path the way ccb_config does: absolute stays
// absolute, relative joins config_dir. NULL in, NULL out.
static char *pv_watch_resolve(const char *config_dir, const char *path) {
  if (path == NULL) {
    return NULL;
  }
  if (path[0] == '/') {
    return ccb_xstrdup(path);
  }
  if (config_dir != NULL && config_dir[0] != '\0') {
    return ccb_path_join(config_dir, path);
  }
  return ccb_xstrdup(path);
}

// Effective config dir: the caller's dir, else the toml's directory,
// else NULL (relative inputs are then used as given).
static char *pv_watch_config_dir(const char *config_dir,
    const char *toml_path) {
  if (config_dir != NULL) {
    return ccb_xstrdup(config_dir);
  }
  if (toml_path != NULL) {
    return ccb_dirname(toml_path);
  }
  return NULL;
}

// ---------- snapshots ----------

static void pv_watch_entry_add(pv_watch_snapshot *snap, const char *path,
    const struct stat *st) {
  pv_watch_entry *e = NULL;
  if (snap->count == snap->cap) {
    size_t ncap = snap->cap == 0 ? 32 : snap->cap * 2;
    snap->entries = ccb_xrealloc(snap->entries,
      ncap * sizeof(*snap->entries));
    snap->cap = ncap;
  }
  e = &snap->entries[snap->count++];
  e->path = ccb_xstrdup(path);
  e->mtime_sec = (long long)st->st_mtim.tv_sec;
  e->mtime_nsec = (long)st->st_mtim.tv_nsec;
  e->size = (long long)st->st_size;
}

static int pv_watch_entry_cmp(const void *a, const void *b) {
  return strcmp(((const pv_watch_entry *)a)->path,
    ((const pv_watch_entry *)b)->path);
}

// Add every regular file under dir to the snapshot, mirroring
// pv_project_load: styles uses the ".css" filter, flow uses no filter
// (all files). A missing/unreadable dir contributes nothing, matching
// preview's tolerant load (ccb_collect_files returns OK with no files
// for a missing dir; a walk error is likewise treated as empty here).
static void pv_watch_add_dir(pv_watch_snapshot *snap, const char *dir,
    const char *ext) {
  char **files = NULL;
  size_t nfiles = 0;
  size_t i = 0;
  if (dir == NULL) {
    return;
  }
  if (ccb_collect_files(dir, ext, &files, &nfiles) != CCB_OK) {
    return;
  }
  for (i = 0; i < nfiles; i++) {
    struct stat st;
    if (stat(files[i], &st) == 0 && S_ISREG(st.st_mode)) {
      pv_watch_entry_add(snap, files[i], &st);
    }
  }
  ccb_free_str_array(files, nfiles);
}

// Build a snapshot from already-resolved paths (no resolution here, so
// both the public take and the context rescan share this exactly).
static pv_watch_snapshot *pv_watch_take_resolved(const char *styles_dir,
    const char *flow_dir, const char *toml_path) {
  pv_watch_snapshot *snap = ccb_xcalloc(1, sizeof(*snap));
  size_t i = 0;
  size_t dst = 0;
  pv_watch_add_dir(snap, styles_dir, ".css");
  pv_watch_add_dir(snap, flow_dir, NULL);
  if (toml_path != NULL) {
    struct stat st;
    if (stat(toml_path, &st) == 0 && S_ISREG(st.st_mode)) {
      pv_watch_entry_add(snap, toml_path, &st);
    }
  }
  // Sort by path, then drop adjacent duplicates (e.g. the toml file
  // sitting inside the styles or flow dir): one entry per path.
  if (snap->count > 1) {
    qsort(snap->entries, snap->count, sizeof(*snap->entries),
      pv_watch_entry_cmp);
  }
  for (i = 0; i < snap->count; i++) {
    if (dst > 0 && strcmp(snap->entries[dst - 1].path,
        snap->entries[i].path) == 0) {
      free(snap->entries[i].path);
      continue;
    }
    snap->entries[dst++] = snap->entries[i];
  }
  snap->count = dst;
  return snap;
}

ccb_status pv_watch_snapshot_take(const char *config_dir,
    const char *styles_dir, const char *flow_dir, const char *toml_path,
    pv_watch_snapshot **out) {
  char *cfg = NULL;
  char *styles = NULL;
  char *flow = NULL;
  char *toml = NULL;
  if (out == NULL) {
    return CCB_ERR_USAGE;
  }
  *out = NULL;
  cfg = pv_watch_config_dir(config_dir, toml_path);
  styles = pv_watch_resolve(cfg, styles_dir);
  flow = pv_watch_resolve(cfg, flow_dir);
  toml = pv_watch_resolve(cfg, toml_path);
  *out = pv_watch_take_resolved(styles, flow, toml);
  free(cfg);
  free(styles);
  free(flow);
  free(toml);
  return CCB_OK;
}

void pv_watch_snapshot_free(pv_watch_snapshot *snap) {
  size_t i = 0;
  if (snap == NULL) {
    return;
  }
  for (i = 0; i < snap->count; i++) {
    free(snap->entries[i].path);
  }
  free(snap->entries);
  free(snap);
}

static int pv_watch_entry_same(const pv_watch_entry *a,
    const pv_watch_entry *b) {
  return a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec &&
    a->size == b->size;
}

int pv_watch_snapshot_changed(const pv_watch_snapshot *old_snap,
    const pv_watch_snapshot *new_snap) {
  size_t i = 0;
  size_t j = 0;
  size_t nold = old_snap != NULL ? old_snap->count : 0;
  size_t nnew = new_snap != NULL ? new_snap->count : 0;
  // Both lists are sorted by path: a merge walk finds additions,
  // removals, and modifications in one pass.
  while (i < nold && j < nnew) {
    int cmp = strcmp(old_snap->entries[i].path,
      new_snap->entries[j].path);
    if (cmp == 0) {
      if (!pv_watch_entry_same(&old_snap->entries[i],
          &new_snap->entries[j])) {
        return 1;
      }
      i++;
      j++;
    } else if (cmp < 0) {
      return 1; // path in old only: removed
    } else {
      return 1; // path in new only: added
    }
  }
  return (i < nold || j < nnew) ? 1 : 0;
}

// ---------- debounce + clock ----------

void pv_watch_debounce_event(pv_watch_debounce *d, long long now_ms) {
  if (d == NULL) {
    return;
  }
  d->last_event_ms = now_ms;
  d->pending = 1;
}

int pv_watch_debounce_due(const pv_watch_debounce *d, long long now_ms) {
  if (d == NULL || !d->pending) {
    return 0;
  }
  return (now_ms - d->last_event_ms) >= PV_WATCH_DEBOUNCE_MS ? 1 : 0;
}

void pv_watch_debounce_reset(pv_watch_debounce *d) {
  if (d == NULL) {
    return;
  }
  d->last_event_ms = 0;
  d->pending = 0;
}

long long pv_watch_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

// ---------- inotify watches (Linux only) ----------

#ifdef __linux__
static void pv_watch_refresh_watches(pv_watch_ctx *ctx) {
  size_t i = 0;
  if (ctx->inotify_fd < 0) {
    return;
  }
  // Drop stale file watches first: a removed file's descriptor is
  // already dead, and rm_watch on it fails harmlessly.
  for (i = 0; i < ctx->nfile_wds; i++) {
    if (ctx->file_wds[i] >= 0) {
      inotify_rm_watch(ctx->inotify_fd, ctx->file_wds[i]);
    }
  }
  ctx->nfile_wds = 0;
  // Directory watches are idempotent: re-adding an existing watch
  // returns its descriptor, so a recreated dir regains its watch here.
  if (ctx->styles_dir != NULL) {
    inotify_add_watch(ctx->inotify_fd, ctx->styles_dir,
      PV_WATCH_DIR_MASK);
  }
  if (ctx->flow_dir != NULL) {
    inotify_add_watch(ctx->inotify_fd, ctx->flow_dir, PV_WATCH_DIR_MASK);
  }
  // One watch per currently-watched file.
  if (ctx->snap != NULL) {
    for (i = 0; i < ctx->snap->count; i++) {
      int wd = inotify_add_watch(ctx->inotify_fd,
        ctx->snap->entries[i].path, PV_WATCH_FILE_MASK);
      if (ctx->nfile_wds == ctx->capfile_wds) {
        size_t ncap = ctx->capfile_wds == 0 ? 32 : ctx->capfile_wds * 2;
        ctx->file_wds = ccb_xrealloc(ctx->file_wds,
          ncap * sizeof(*ctx->file_wds));
        ctx->capfile_wds = ncap;
      }
      ctx->file_wds[ctx->nfile_wds++] = wd;
    }
  }
}

// Drain all queued events (the fd is non-blocking). Content is ignored:
// the rescan after the drain is the source of truth.
static void pv_watch_drain(int fd) {
  char buf[4096];
  ssize_t n = 0;
  do {
    n = read(fd, buf, sizeof(buf));
  } while (n > 0);
}
#else
static void pv_watch_refresh_watches(pv_watch_ctx *ctx) {
  CCB_UNUSED(ctx);
}
#endif

// ---------- context ----------

// Rescan and adopt the new snapshot when it differs. Returns 1 on a
// change (snapshot + watches updated), 0 when nothing changed.
static int pv_watch_rescan(pv_watch_ctx *ctx) {
  pv_watch_snapshot *fresh = pv_watch_take_resolved(ctx->styles_dir,
    ctx->flow_dir, ctx->toml_path);
  int changed = pv_watch_snapshot_changed(ctx->snap, fresh);
  if (!changed) {
    pv_watch_snapshot_free(fresh);
    return 0;
  }
  pv_watch_snapshot_free(ctx->snap);
  ctx->snap = fresh;
  pv_watch_refresh_watches(ctx);
  return 1;
}

ccb_status pv_watch_begin(const char *config_dir, const char *styles_dir,
    const char *flow_dir, const char *toml_path, pv_watch_ctx **out,
    int *used_inotify_out) {
  pv_watch_ctx *ctx = NULL;
  char *cfg = NULL;
  if (out == NULL || used_inotify_out == NULL) {
    return CCB_ERR_USAGE;
  }
  *out = NULL;
  *used_inotify_out = 0;
  ctx = ccb_xcalloc(1, sizeof(*ctx));
  ctx->inotify_fd = -1;
  cfg = pv_watch_config_dir(config_dir, toml_path);
  ctx->styles_dir = pv_watch_resolve(cfg, styles_dir);
  ctx->flow_dir = pv_watch_resolve(cfg, flow_dir);
  ctx->toml_path = pv_watch_resolve(cfg, toml_path);
  free(cfg);
  ctx->snap = pv_watch_take_resolved(ctx->styles_dir, ctx->flow_dir,
    ctx->toml_path);
#ifdef __linux__
  {
    // inotify_init (not init1) + fcntl: declared under the POSIX
    // feature macros ccb_common.h already sets, no _GNU_SOURCE needed.
    int fd = inotify_init();
    if (fd >= 0) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      ctx->inotify_fd = fd;
      ctx->used_inotify = 1;
      pv_watch_refresh_watches(ctx);
    }
  }
#endif
  *used_inotify_out = ctx->used_inotify;
  *out = ctx;
  return CCB_OK;
}

int pv_watch_wait(pv_watch_ctx *ctx, long timeout_ms) {
  long remaining = 0;
  long long deadline = 0;
  if (ctx == NULL) {
    return -1;
  }
  // A negative timeout behaves like 0: one immediate check, no block,
  // so this function never blocks forever for any timeout value.
  remaining = timeout_ms < 0 ? 0 : timeout_ms;
  deadline = pv_watch_now_ms() + (long long)remaining;
#ifdef __linux__
  if (ctx->inotify_fd >= 0) {
    // inotify is a wakeup: drain, rescan, and report only a real diff.
    // Spurious events (IN_IGNORED queued when a watch refresh removes
    // a stale descriptor, an untracked file in a watched dir, an
    // attrib-only change) are consumed and waiting continues until the
    // deadline, so a refresh can never make the next wait report a
    // change that is not there. The deadline is absolute, so an event
    // flood cannot extend the wait past timeout_ms (plus one rescan).
    for (;;) {
      long long left = deadline - pv_watch_now_ms();
      struct pollfd pfd;
      long poll_timeout = 0;
      int rc = 0;
      if (left <= 0) {
        return pv_watch_rescan(ctx);
      }
      poll_timeout = left > 2147483647LL ? 2147483647L : (long)left;
      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = ctx->inotify_fd;
      pfd.events = POLLIN;
      rc = poll(&pfd, 1, (int)poll_timeout);
      if (rc > 0) {
        pv_watch_drain(ctx->inotify_fd);
        if (pv_watch_rescan(ctx)) {
          return 1;
        }
        continue;
      }
      if (rc == 0) {
        // Timeout: rescan anyway, so an event lost between watch setup
        // and this call (or a recreated directory with no watch yet)
        // still surfaces through the snapshot diff.
        return pv_watch_rescan(ctx);
      }
      if (errno == EINTR) {
        return 0;
      }
      return -1;
    }
  }
#else
  CCB_UNUSED(deadline);
#endif
  // Polling backend (and non-Linux): rescan in slices until the
  // timeout expires, so a change is reported promptly without a busy
  // loop and without ever overshooting the timeout by much.
  {
    long long start = pv_watch_now_ms();
    for (;;) {
      long long elapsed = 0;
      long slice = 0;
      int rc = 0;
      if (pv_watch_rescan(ctx)) {
        return 1;
      }
      elapsed = pv_watch_now_ms() - start;
      if (elapsed >= (long long)remaining) {
        return 0;
      }
      slice = (long)(remaining - elapsed);
      if (slice > PV_WATCH_POLL_SLICE_MS) {
        slice = PV_WATCH_POLL_SLICE_MS;
      }
      rc = poll(NULL, 0, (int)slice);
      if (rc < 0) {
        if (errno == EINTR) {
          return 0;
        }
        return -1;
      }
    }
  }
}

void pv_watch_end(pv_watch_ctx *ctx) {
  if (ctx == NULL) {
    return;
  }
#ifdef __linux__
  if (ctx->inotify_fd >= 0) {
    close(ctx->inotify_fd);
  }
#endif
  // File watch descriptors die with the fd; only the array is freed.
  free(ctx->file_wds);
  pv_watch_snapshot_free(ctx->snap);
  free(ctx->styles_dir);
  free(ctx->flow_dir);
  free(ctx->toml_path);
  free(ctx);
}
