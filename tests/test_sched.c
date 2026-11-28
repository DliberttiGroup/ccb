// test_sched: unit tests for the parallel task executor (ccb_sched,
// Step 8) and the single-writer cache publication it relies on
// (ccb_cache_delta). Fast: no real Android build.
//
// Stages, in the order the scheduler proves them:
//   1. single-worker execution matches the sequential executor
//      (same order, same states) + validation failures spawn no
//      thread.
//   2. two independent tasks execute concurrently (a pthread
//      barrier of 2 releases only when both arrived — no sleeps,
//      no timing assertions).
//   3. dependency gating and failure propagation (deterministic
//      with 1 worker; terminal states that hold at any worker
//      count with 4).
//   4. cancellation (a gate + helper thread, no sleeps), shutdown
//      (run returns only after joining every worker), stress
//      (64 independent tasks, 4 workers, first 4 rendezvous at a
//      barrier of 4).
//   5. cache behavior under concurrent execution: deltas are
//      per-task, merged once by a single writer, and a failed
//      task's delta is discarded, never published.
// Stage 6 — parallel execution of real build tasks — is NOT
// reached and has no test here: real steps still share one log,
// one diag list, one cache, and one pipe (see ccb_sched.h).
#include "test.h"
#include "ccb_cache.h"
#include "ccb_graph.h"
#include "ccb_sched.h"
#include <pthread.h>

// ---------- shared recorder ----------

// One ctx for all scheduler tests: a mutex-guarded run-order
// record plus the gate/cancel handshake. Run functions touch only
// this ctx (never the shared log/diag/cache), per the parallel
// contract in ccb_sched.h.
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cond;
  char seen[1024];
  size_t nseen;
  long count; // tasks that ran (under mu)
  int started; // gate task started (cancel test)
  int release; // gate task may proceed (cancel test)
  pthread_barrier_t barrier; // rendezvous (concurrency tests)
  int use_barrier;
} sched_ctx;

static void sched_ctx_init(sched_ctx *c) {
  memset(c, 0, sizeof(*c));
  pthread_mutex_init(&c->mu, NULL);
  pthread_cond_init(&c->cond, NULL);
}

static void sched_ctx_free(sched_ctx *c) {
  pthread_mutex_destroy(&c->mu);
  pthread_cond_destroy(&c->cond);
  if (c->use_barrier) {
    pthread_barrier_destroy(&c->barrier);
  }
}

static void record_locked(sched_ctx *c, const char *id) {
  size_t len = strlen(id);
  if (c->nseen + len + 1 < sizeof(c->seen)) {
    memcpy(c->seen + c->nseen, id, len);
    c->nseen += len;
    c->seen[c->nseen++] = ' ';
    c->seen[c->nseen] = '\0';
  }
  c->count++;
}

static ccb_status record_run(void *ctx, ccb_task *t) {
  sched_ctx *c = ctx;
  if (c->use_barrier && t->id[0] == 's' && t->id[1] >= '0' &&
      t->id[1] <= '3' && t->id[2] == '\0') {
    // Stress-test rendezvous: the first four tasks must all be
    // running at once for this barrier to release.
    pthread_barrier_wait(&c->barrier);
  }
  pthread_mutex_lock(&c->mu);
  record_locked(c, t->id);
  pthread_mutex_unlock(&c->mu);
  return CCB_OK;
}

static ccb_status fail_run(void *ctx, ccb_task *t) {
  sched_ctx *c = ctx;
  pthread_mutex_lock(&c->mu);
  record_locked(c, t->id);
  pthread_mutex_unlock(&c->mu);
  return CCB_ERR_BUILD;
}

static ccb_status skip_run(void *ctx, ccb_task *t) {
  sched_ctx *c = ctx;
  pthread_mutex_lock(&c->mu);
  record_locked(c, t->id);
  pthread_mutex_unlock(&c->mu);
  t->skipped = 1;
  return CCB_OK;
}

// Blocks on the ctx gate until the helper thread cancels the
// scheduler and releases it. No sleep: the handshake is condvars.
static ccb_status gate_run(void *ctx, ccb_task *t) {
  sched_ctx *c = ctx;
  pthread_mutex_lock(&c->mu);
  record_locked(c, t->id);
  c->started = 1;
  pthread_cond_broadcast(&c->cond);
  while (!c->release) {
    pthread_cond_wait(&c->cond, &c->mu);
  }
  pthread_mutex_unlock(&c->mu);
  return CCB_OK;
}

// Helper thread for the cancel test: waits until the gate task
// started, cancels the scheduler, then releases the gate.
typedef struct {
  sched_ctx *c;
  ccb_sched *s;
} cancel_helper_arg;

static void *cancel_helper(void *arg) {
  cancel_helper_arg *a = arg;
  pthread_mutex_lock(&a->c->mu);
  while (!a->c->started) {
    pthread_cond_wait(&a->c->cond, &a->c->mu);
  }
  pthread_mutex_unlock(&a->c->mu);
  ccb_sched_cancel(a->s);
  pthread_mutex_lock(&a->c->mu);
  a->c->release = 1;
  pthread_cond_broadcast(&a->c->cond);
  pthread_mutex_unlock(&a->c->mu);
  return NULL;
}

// Two independent tasks that rendezvous at a barrier of 2: the
// barrier releases only when both are running at once.
static ccb_status barrier_run(void *ctx, ccb_task *t) {
  sched_ctx *c = ctx;
  pthread_barrier_wait(&c->barrier);
  pthread_mutex_lock(&c->mu);
  record_locked(c, t->id);
  pthread_mutex_unlock(&c->mu);
  return CCB_OK;
}

// ---------- graph builders ----------

static ccb_graph *diamond_graph(ccb_task_run_fn run) {
  ccb_graph *g = ccb_graph_create();
  ccb_task *a = ccb_graph_add_task(g, "a", "test",
    CCB_TASK_CACHE_FINGERPRINT, run, NULL);
  ccb_task *b = ccb_graph_add_task(g, "b", "test",
    CCB_TASK_CACHE_FINGERPRINT, run, NULL);
  ccb_task *c = ccb_graph_add_task(g, "c", "test",
    CCB_TASK_CACHE_FINGERPRINT, run, NULL);
  ccb_task *d = ccb_graph_add_task(g, "d", "test",
    CCB_TASK_CACHE_FINGERPRINT, run, NULL);
  ccb_task_add_dep(b, "a");
  ccb_task_add_dep(c, "a");
  ccb_task_add_dep(d, "b");
  ccb_task_add_dep(d, "c");
  CCB_UNUSED(a);
  return g;
}

// Position of id in the space-separated order record, or -1.
static long order_pos(const char *seen, const char *id) {
  size_t idlen = strlen(id);
  const char *p = seen;
  while (*p != '\0') {
    if (strncmp(p, id, idlen) == 0 && p[idlen] == ' ') {
      return (long)(p - seen);
    }
    const char *sp = strchr(p, ' ');
    if (sp == NULL) {
      return -1;
    }
    p = sp + 1;
  }
  return -1;
}

int main(void) {
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_log log;
  ccb_log_init(&log, 0, 0);

  // --- stage 1: one worker matches the sequential executor ---
  {
    sched_ctx c1;
    sched_ctx_init(&c1);
    ccb_graph *g = diamond_graph(record_run);
    ccb_diag_list ed;
    ccb_diag_list_init(&ed);
    CHECK(ccb_graph_execute(g, &c1, &log, &ed) == CCB_OK);
    char seq_seen[1024];
    snprintf(seq_seen, sizeof(seq_seen), "%s", c1.seen);
    ccb_diag_list_free(&ed);
    ccb_graph_free(g);
    sched_ctx c2;
    sched_ctx_init(&c2);
    g = diamond_graph(record_run);
    CHECK(ccb_graph_execute_parallel(g, &c2, &log, &diags, 1) ==
      CCB_OK);
    CHECK_STR_EQ(c2.seen, seq_seen);
    CHECK_STR_EQ(c2.seen, "a b c d ");
    for (size_t i = 0; i < g->count; i++) {
      CHECK(g->tasks[i]->state == CCB_TASK_DONE);
    }
    ccb_graph_free(g);
    sched_ctx_free(&c1);
    sched_ctx_free(&c2);
  }

  // --- stage 1: a cycle fails validation, no thread spawns ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    CCB_UNUSED(a);
    CCB_UNUSED(b);
    ccb_task_add_dep(a, "b");
    ccb_task_add_dep(b, "a");
    sched_ctx c;
    sched_ctx_init(&c);
    ccb_diag_list ed;
    ccb_diag_list_init(&ed);
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &ed, 4) ==
      CCB_ERR_CONFIG);
    CHECK(c.count == 0);
    CHECK(a->state == CCB_TASK_WAITING);
    ccb_diag_list_free(&ed);
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 1: zero workers is a usage error ---
  {
    ccb_graph *g = diamond_graph(record_run);
    sched_ctx c;
    sched_ctx_init(&c);
    size_t before = diags.count;
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 0) ==
      CCB_ERR_USAGE);
    CHECK(diags.count == before + 1);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1016");
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 1: empty graph runs, skipped tasks report skipped ---
  {
    ccb_graph *g = ccb_graph_create();
    sched_ctx c;
    sched_ctx_init(&c);
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 4) ==
      CCB_OK);
    ccb_graph_free(g);
    sched_ctx_free(&c);
    g = ccb_graph_create();
    ccb_graph_add_task(g, "only", "test",
      CCB_TASK_CACHE_FINGERPRINT, skip_run, NULL);
    sched_ctx_init(&c);
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 2) ==
      CCB_OK);
    CHECK(ccb_graph_find(g, "only")->state == CCB_TASK_DONE);
    CHECK(ccb_graph_find(g, "only")->skipped == 1);
    // Parallel mode has no shared log stash to read a skip reason
    // from (see ccb_sched.h): skip_reason stays NULL.
    CHECK(ccb_graph_find(g, "only")->skip_reason == NULL);
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 2: two independent tasks run concurrently ---
  {
    sched_ctx c;
    sched_ctx_init(&c);
    c.use_barrier = 1;
    pthread_barrier_init(&c.barrier, NULL, 2);
    ccb_graph *g = ccb_graph_create();
    ccb_graph_add_task(g, "x", "test",
      CCB_TASK_CACHE_FINGERPRINT, barrier_run, NULL);
    ccb_graph_add_task(g, "y", "test",
      CCB_TASK_CACHE_FINGERPRINT, barrier_run, NULL);
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 2) ==
      CCB_OK);
    CHECK(c.count == 2);
    CHECK(ccb_graph_find(g, "x")->state == CCB_TASK_DONE);
    CHECK(ccb_graph_find(g, "y")->state == CCB_TASK_DONE);
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 3: dependency gating (diamond, 4 workers) ---
  {
    sched_ctx c;
    sched_ctx_init(&c);
    ccb_graph *g = diamond_graph(record_run);
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 4) ==
      CCB_OK);
    CHECK(order_pos(c.seen, "a") == 0);
    CHECK(order_pos(c.seen, "b") > order_pos(c.seen, "a"));
    CHECK(order_pos(c.seen, "c") > order_pos(c.seen, "a"));
    CHECK(order_pos(c.seen, "d") > order_pos(c.seen, "b"));
    CHECK(order_pos(c.seen, "d") > order_pos(c.seen, "c"));
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 3: failure propagation, 1 worker (deterministic) ---
  // Order a, e, b(fail), c, d: e is independent and runs before
  // the failure; c and d depend on b and never start.
  {
    sched_ctx c;
    sched_ctx_init(&c);
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *e = ccb_graph_add_task(g, "e", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, fail_run, NULL);
    ccb_task *cc = ccb_graph_add_task(g, "c", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *d = ccb_graph_add_task(g, "d", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    CCB_UNUSED(a);
    CCB_UNUSED(e);
    CCB_UNUSED(b);
    CCB_UNUSED(cc);
    CCB_UNUSED(d);
    ccb_task_add_dep(b, "a");
    ccb_task_add_dep(cc, "b");
    ccb_task_add_dep(d, "b");
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 1) ==
      CCB_ERR_BUILD);
    CHECK_STR_EQ(c.seen, "a e b ");
    CHECK(a->state == CCB_TASK_DONE);
    CHECK(e->state == CCB_TASK_DONE);
    CHECK(b->state == CCB_TASK_FAILED);
    CHECK(cc->state == CCB_TASK_CANCELLED);
    CHECK(d->state == CCB_TASK_CANCELLED);
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 3: failure propagation, 4 workers (timing-free) ---
  // Only states that hold at any worker count are asserted: b
  // fails, so d (which needs b DONE) can never run; a has no
  // dependencies, so it always runs first. c and e may run or be
  // cancelled depending on timing, so they are not asserted.
  {
    sched_ctx c;
    sched_ctx_init(&c);
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, fail_run, NULL);
    ccb_task *cc = ccb_graph_add_task(g, "c", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *d = ccb_graph_add_task(g, "d", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *e = ccb_graph_add_task(g, "e", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    CCB_UNUSED(a);
    CCB_UNUSED(b);
    CCB_UNUSED(cc);
    CCB_UNUSED(d);
    CCB_UNUSED(e);
    ccb_task_add_dep(b, "a");
    ccb_task_add_dep(cc, "a");
    ccb_task_add_dep(d, "b");
    ccb_task_add_dep(d, "c");
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 4) ==
      CCB_ERR_BUILD);
    CHECK(a->state == CCB_TASK_DONE);
    CHECK(b->state == CCB_TASK_FAILED);
    CHECK(d->state == CCB_TASK_CANCELLED);
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 4: cancellation mid-run, 1 worker (deterministic) ---
  // A blocks on the gate; the helper cancels while A runs, then
  // releases it. A finishes DONE; B (depends on A) and C
  // (independent, queued behind A) never start.
  {
    sched_ctx c;
    sched_ctx_init(&c);
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, gate_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *cc = ccb_graph_add_task(g, "c", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    CCB_UNUSED(a);
    CCB_UNUSED(b);
    CCB_UNUSED(cc);
    ccb_task_add_dep(b, "a");
    ccb_sched *s = ccb_sched_create(g, &c, &log, &diags, 1);
    CHECK(s != NULL);
    cancel_helper_arg arg;
    arg.c = &c;
    arg.s = s;
    pthread_t helper;
    CHECK(pthread_create(&helper, NULL, cancel_helper, &arg) == 0);
    CHECK(ccb_sched_run(s) == CCB_OK);
    CHECK(pthread_join(helper, NULL) == 0);
    CHECK(a->state == CCB_TASK_DONE);
    CHECK(b->state == CCB_TASK_CANCELLED);
    CHECK(cc->state == CCB_TASK_CANCELLED);
    CHECK(c.count == 1);
    ccb_sched_free(s);
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 4: cancel before run starts: nothing runs ---
  {
    sched_ctx c;
    sched_ctx_init(&c);
    ccb_graph *g = diamond_graph(record_run);
    ccb_sched *s = ccb_sched_create(g, &c, &log, &diags, 4);
    CHECK(s != NULL);
    ccb_sched_cancel(s);
    CHECK(ccb_sched_run(s) == CCB_OK);
    for (size_t i = 0; i < g->count; i++) {
      CHECK(g->tasks[i]->state == CCB_TASK_CANCELLED);
    }
    CHECK(c.count == 0);
    ccb_sched_free(s);
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 4: stress, 64 independent tasks, 4 workers ---
  {
    sched_ctx c;
    sched_ctx_init(&c);
    c.use_barrier = 1;
    pthread_barrier_init(&c.barrier, NULL, 4);
    ccb_graph *g = ccb_graph_create();
    for (int i = 0; i < 64; i++) {
      char id[8];
      snprintf(id, sizeof(id), "s%d", i);
      CHECK(ccb_graph_add_task(g, id, "test",
        CCB_TASK_CACHE_FINGERPRINT, record_run, NULL) != NULL);
    }
    CHECK(ccb_graph_execute_parallel(g, &c, &log, &diags, 4) ==
      CCB_OK);
    CHECK(c.count == 64);
    for (size_t i = 0; i < g->count; i++) {
      CHECK(g->tasks[i]->state == CCB_TASK_DONE);
      CHECK(g->tasks[i]->duration_ms >= 0);
    }
    ccb_graph_free(g);
    sched_ctx_free(&c);
  }

  // --- stage 5: cache deltas are per-task, merged once, by one writer ---
  {
    ccb_cache *cache = ccb_xcalloc(1, sizeof(*cache));
    ccb_cache_delta d1;
    ccb_cache_delta_init(&d1);
    ccb_cache_delta_set_step(&d1, "module-base", "fp1");
    ccb_cache_delta_set_step(&d1, "module-core", "fp2");
    ccb_cache_delta d2;
    ccb_cache_delta_init(&d2);
    ccb_cache_delta_set_step(&d2, "module-core", "fp2-new");
    // A task's delta is invisible until merged: building it alone
    // publishes nothing.
    CHECK(ccb_cache_step_fp(cache, "module-base") == NULL);
    ccb_cache_delta_merge(cache, &d1);
    CHECK_STR_EQ(ccb_cache_step_fp(cache, "module-base"), "fp1");
    CHECK_STR_EQ(ccb_cache_step_fp(cache, "module-core"), "fp2");
    // A later merge of the same step wins (completion order).
    ccb_cache_delta_merge(cache, &d2);
    CHECK_STR_EQ(ccb_cache_step_fp(cache, "module-core"), "fp2-new");
    // A failed task's delta is discarded, never merged: the cache
    // never sees its step.
    ccb_cache_delta d3;
    ccb_cache_delta_init(&d3);
    ccb_cache_delta_set_step(&d3, "module-broken", "fpX");
    ccb_cache_delta_free(&d3);
    CHECK(ccb_cache_step_fp(cache, "module-broken") == NULL);
    ccb_cache_delta_free(&d1);
    ccb_cache_delta_free(&d2);
    ccb_cache_free(cache);
  }

  ccb_diag_list_free(&diags);
  TEST_MAIN_END();
}
