// ccb_sched implementation: pthread worker pool over a ccb_graph.
// See ccb_sched.h for the contract.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_sched.h"
#include <pthread.h>

struct ccb_sched {
  ccb_graph *g;
  void *ctx;
  ccb_log *log;
  ccb_diag_list *diags;
  size_t workers;
  pthread_t *threads;
  pthread_mutex_t mu;
  pthread_cond_t cond;
  // Ready queue: FIFO of task indices, insertion order among the
  // tasks that become ready together.
  size_t *queue;
  size_t qhead;
  size_t qtail;
  size_t qcap;
  size_t *pending; // per task: deps not DONE yet
  size_t remaining; // tasks not yet terminal (DONE/FAILED/CANCELLED)
  int failed; // a task failed; no new task starts
  int cancelled; // ccb_sched_cancel was called
  ccb_status first_status; // the first failing task's status
};

// Caller holds mu. Enqueue a task index; grows the queue.
static void ccb_sched_enqueue(ccb_sched *s, size_t idx) {
  if (s->qtail == s->qcap) {
    size_t ncap = s->qcap == 0 ? 16 : s->qcap * 2;
    s->queue = ccb_xrealloc(s->queue, ncap * sizeof(*s->queue));
    s->qcap = ncap;
  }
  s->queue[s->qtail++] = idx;
}

// Caller holds mu. Mark every WAITING or READY task CANCELLED and
// count it terminal. READY entries stay in the queue and are
// skipped on dequeue (their state says CANCELLED).
static void ccb_sched_cancel_locked(ccb_sched *s) {
  for (size_t i = 0; i < s->g->count; i++) {
    ccb_task *t = s->g->tasks[i];
    if (t->state != CCB_TASK_WAITING && t->state != CCB_TASK_READY) {
      continue;
    }
    t->state = CCB_TASK_CANCELLED;
    s->remaining--;
  }
  pthread_cond_broadcast(&s->cond);
}

static void *ccb_sched_worker(void *arg) {
  ccb_sched *s = arg;
  for (;;) {
    pthread_mutex_lock(&s->mu);
    while (s->qhead == s->qtail && s->remaining > 0 &&
        !s->failed && !s->cancelled) {
      pthread_cond_wait(&s->cond, &s->mu);
    }
    if (s->remaining == 0) {
      pthread_mutex_unlock(&s->mu);
      return NULL;
    }
    // A failure or cancel leaves ready tasks in the queue, but
    // they were all marked CANCELLED already; skip them. When the
    // queue drains without new work, the remaining==0 check above
    // ends the worker (remaining hits 0 when the last RUNNING task
    // lands, or was already 0 after the cancel marking).
    ccb_task *t = NULL;
    while (s->qhead < s->qtail) {
      size_t cand = s->queue[s->qhead++];
      if (s->g->tasks[cand]->state == CCB_TASK_READY) {
        t = s->g->tasks[cand];
        break;
      }
    }
    if (t == NULL) {
      if (s->remaining == 0) {
        pthread_mutex_unlock(&s->mu);
        return NULL;
      }
      // No runnable task right now (the rest are RUNNING or were
      // cancelled out of the queue): wait for a completion.
      pthread_cond_wait(&s->cond, &s->mu);
      pthread_mutex_unlock(&s->mu);
      continue;
    }
    t->state = CCB_TASK_RUNNING;
    if (s->log != NULL) {
      ccb_log_task_start(s->log, t->id);
    }
    pthread_mutex_unlock(&s->mu);
    long t0 = ccb_graph_now_ms();
    ccb_status rc = t->run != NULL ? t->run(s->ctx, t) : CCB_OK;
    t->duration_ms = ccb_graph_now_ms() - t0;
    pthread_mutex_lock(&s->mu);
    if (rc == CCB_OK) {
      t->state = CCB_TASK_DONE;
      if (s->log != NULL) {
        ccb_log_task_done(s->log, t->id, t->duration_ms, t->skipped,
          NULL);
      }
      // Wake dependents whose last pending dependency just landed.
      // A failure elsewhere may already have cancelled them; only
      // WAITING tasks with no pending deps become READY.
      if (!s->failed && !s->cancelled) {
        for (size_t i = 0; i < s->g->count; i++) {
          ccb_task *d = s->g->tasks[i];
          if (d->state != CCB_TASK_WAITING) {
            continue;
          }
          int depends = 0;
          for (size_t j = 0; j < d->deps_count; j++) {
            if (strcmp(d->deps[j], t->id) == 0) {
              depends = 1;
              break;
            }
          }
          if (!depends) {
            continue;
          }
          if (s->pending[i] > 0) {
            s->pending[i]--;
          }
          if (s->pending[i] == 0) {
            d->state = CCB_TASK_READY;
            ccb_sched_enqueue(s, i);
          }
        }
      }
    } else {
      t->state = CCB_TASK_FAILED;
      if (s->log != NULL) {
        ccb_log_task_failed(s->log, t->id, t->duration_ms);
      }
      if (!s->failed) {
        s->first_status = rc;
      }
      s->failed = 1;
      ccb_sched_cancel_locked(s);
    }
    s->remaining--;
    if (s->remaining == 0) {
      pthread_cond_broadcast(&s->cond);
    } else if (rc == CCB_OK) {
      pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->mu);
  }
}

ccb_sched *ccb_sched_create(ccb_graph *g, void *ctx, ccb_log *log,
    ccb_diag_list *diags, size_t workers) {
  if (workers == 0) {
    if (diags != NULL) {
      ccb_diag d = ccb_diag_make("CCB-E1016", "graph",
        "parallel executor needs at least 1 worker");
      ccb_diag_list_push(diags, &d);
    }
    return NULL;
  }
  ccb_sched *s = ccb_xcalloc(1, sizeof(*s));
  s->g = g;
  s->ctx = ctx;
  s->log = log;
  s->diags = diags;
  s->workers = workers;
  s->first_status = CCB_OK;
  pthread_mutex_init(&s->mu, NULL);
  pthread_cond_init(&s->cond, NULL);
  return s;
}

void ccb_sched_free(ccb_sched *s) {
  if (s == NULL) {
    return;
  }
  pthread_mutex_destroy(&s->mu);
  pthread_cond_destroy(&s->cond);
  free(s->threads);
  free(s->queue);
  free(s->pending);
  free(s);
}

void ccb_sched_cancel(ccb_sched *s) {
  pthread_mutex_lock(&s->mu);
  if (!s->cancelled) {
    s->cancelled = 1;
    // remaining is 0 before run starts (pending not computed yet),
    // so there is nothing to mark until run initializes it; the
    // run loop checks the flag before dequeuing anything.
    if (s->pending != NULL) {
      ccb_sched_cancel_locked(s);
    }
  }
  pthread_mutex_unlock(&s->mu);
}

ccb_status ccb_sched_run(ccb_sched *s) {
  if (ccb_graph_validate(s->g, s->diags) != CCB_OK) {
    return CCB_ERR_CONFIG;
  }
  size_t n = s->g->count;
  s->pending = ccb_xcalloc(n == 0 ? 1 : n, sizeof(*s->pending));
  s->remaining = 0;
  for (size_t i = 0; i < n; i++) {
    ccb_task *t = s->g->tasks[i];
    if (t->state == CCB_TASK_DONE) {
      continue;
    }
    s->remaining++;
    size_t pend = 0;
    for (size_t j = 0; j < t->deps_count; j++) {
      ccb_task *d = ccb_graph_find(s->g, t->deps[j]);
      if (d != NULL && d->state != CCB_TASK_DONE) {
        pend++;
      }
    }
    s->pending[i] = pend;
    if (pend == 0) {
      t->state = CCB_TASK_READY;
      ccb_sched_enqueue(s, i);
    }
  }
  // A cancel that arrived before run: nothing starts.
  if (s->cancelled) {
    ccb_sched_cancel_locked(s);
  }
  size_t nthreads = s->workers;
  if (nthreads > s->remaining) {
    nthreads = s->remaining;
  }
  s->threads = ccb_xcalloc(nthreads == 0 ? 1 : nthreads,
    sizeof(*s->threads));
  size_t spawned = 0;
  for (size_t i = 0; i < nthreads; i++) {
    if (pthread_create(&s->threads[i], NULL, ccb_sched_worker, s) != 0) {
      break;
    }
    spawned++;
  }
  if (spawned < nthreads) {
    // Could not spawn the full pool: cancel the rest and join what
    // started, rather than running short-handed and silently.
    ccb_sched_cancel(s);
    for (size_t i = 0; i < spawned; i++) {
      pthread_join(s->threads[i], NULL);
    }
    return CCB_ERR_IO;
  }
  for (size_t i = 0; i < nthreads; i++) {
    pthread_join(s->threads[i], NULL);
  }
  return s->failed ? s->first_status : CCB_OK;
}

ccb_status ccb_graph_execute_parallel(ccb_graph *g, void *ctx,
    ccb_log *log, ccb_diag_list *diags, size_t workers) {
  ccb_sched *s = ccb_sched_create(g, ctx, log, diags, workers);
  if (s == NULL) {
    return CCB_ERR_USAGE;
  }
  ccb_status rc = ccb_sched_run(s);
  ccb_sched_free(s);
  return rc;
}
