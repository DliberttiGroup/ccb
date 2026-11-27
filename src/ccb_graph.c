// ccb_graph implementation: task graph construction, validation,
// cycle detection, planning, affected-task calculation, and the
// sequential executor. See ccb_graph.h for the contract.
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_json.h"
#include "ccb_graph.h"
#include <time.h>
#include <unistd.h>

const char *ccb_task_state_name(ccb_task_state s) {
  switch (s) {
    case CCB_TASK_WAITING: return "waiting";
    case CCB_TASK_READY: return "ready";
    case CCB_TASK_RUNNING: return "running";
    case CCB_TASK_DONE: return "done";
    case CCB_TASK_FAILED: return "failed";
    case CCB_TASK_CANCELLED: return "cancelled";
    default: return "unknown";
  }
}

long ccb_graph_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + (long)ts.tv_nsec / 1000000;
}

ccb_graph *ccb_graph_create(void) {
  return ccb_xcalloc(1, sizeof(ccb_graph));
}

static void ccb_task_free(ccb_task *t) {
  if (t == NULL) {
    return;
  }
  free(t->id);
  free(t->type);
  for (size_t i = 0; i < t->inputs_count; i++) {
    free(t->inputs[i].name);
  }
  free(t->inputs);
  ccb_free_str_array(t->outputs, t->outputs_count);
  ccb_free_str_array(t->deps, t->deps_count);
  free(t);
}

void ccb_graph_free(ccb_graph *g) {
  if (g == NULL) {
    return;
  }
  for (size_t i = 0; i < g->count; i++) {
    ccb_task_free(g->tasks[i]);
  }
  free(g->tasks);
  free(g);
}

ccb_task *ccb_graph_find(ccb_graph *g, const char *id) {
  for (size_t i = 0; i < g->count; i++) {
    if (strcmp(g->tasks[i]->id, id) == 0) {
      return g->tasks[i];
    }
  }
  return NULL;
}

// Index of the task with this ID, or (size_t)-1 when absent.
static size_t ccb_graph_index(ccb_graph *g, const char *id) {
  for (size_t i = 0; i < g->count; i++) {
    if (strcmp(g->tasks[i]->id, id) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

static void ccb_graph_diag(ccb_diag_list *diags, const char *code,
    const char *msg) {
  if (diags == NULL) {
    return;
  }
  ccb_diag d = ccb_diag_make(code, "graph", msg);
  ccb_diag_list_push(diags, &d);
}

ccb_task *ccb_graph_add_task(ccb_graph *g, const char *id,
    const char *type, ccb_task_cache cache, ccb_task_run_fn run,
    ccb_diag_list *diags) {
  if (ccb_graph_find(g, id) != NULL) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "duplicate task id '");
    ccb_str_append(&msg, id);
    ccb_str_append(&msg, "'");
    ccb_graph_diag(diags, "CCB-E1012", msg.buf);
    ccb_str_free(&msg);
    return NULL;
  }
  if (g->count == g->cap) {
    size_t ncap = g->cap == 0 ? 8 : g->cap * 2;
    g->tasks = ccb_xrealloc(g->tasks, ncap * sizeof(*g->tasks));
    g->cap = ncap;
  }
  ccb_task *t = ccb_xcalloc(1, sizeof(*t));
  t->id = ccb_xstrdup(id);
  t->type = ccb_xstrdup(type);
  t->state = CCB_TASK_WAITING;
  t->cache = cache;
  t->run = run;
  t->duration_ms = -1;
  g->tasks[g->count++] = t;
  return t;
}

void ccb_task_add_input(ccb_task *t, const char *name,
    ccb_task_input_kind kind) {
  if (t->inputs_count == t->inputs_cap) {
    size_t ncap = t->inputs_cap == 0 ? 8 : t->inputs_cap * 2;
    t->inputs = ccb_xrealloc(t->inputs, ncap * sizeof(*t->inputs));
    t->inputs_cap = ncap;
  }
  t->inputs[t->inputs_count].name = ccb_xstrdup(name);
  t->inputs[t->inputs_count].kind = kind;
  t->inputs_count++;
}

void ccb_task_add_output(ccb_task *t, const char *name) {
  if (t->outputs_count == t->outputs_cap) {
    size_t ncap = t->outputs_cap == 0 ? 4 : t->outputs_cap * 2;
    t->outputs = ccb_xrealloc(t->outputs,
      ncap * sizeof(*t->outputs));
    t->outputs_cap = ncap;
  }
  t->outputs[t->outputs_count++] = ccb_xstrdup(name);
}

void ccb_task_add_dep(ccb_task *t, const char *dep_id) {
  if (t->deps_count == t->deps_cap) {
    size_t ncap = t->deps_cap == 0 ? 4 : t->deps_cap * 2;
    t->deps = ccb_xrealloc(t->deps, ncap * sizeof(*t->deps));
    t->deps_cap = ncap;
  }
  t->deps[t->deps_count++] = ccb_xstrdup(dep_id);
}

// ---------- validation ----------

// A task other than self claims this output name.
static int ccb_has_producer(ccb_graph *g, ccb_task *self,
    const char *name) {
  for (size_t i = 0; i < g->count; i++) {
    ccb_task *t = g->tasks[i];
    if (t == self) {
      continue;
    }
    for (size_t j = 0; j < t->outputs_count; j++) {
      if (strcmp(t->outputs[j], name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static void ccb_vdiag_task(ccb_diag_list *diags, const char *code,
    const char *task_id, const char *what, const char *detail) {
  ccb_str msg;
  ccb_str_init(&msg);
  ccb_str_append(&msg, "task '");
  ccb_str_append(&msg, task_id);
  ccb_str_append(&msg, "' ");
  ccb_str_append(&msg, what);
  ccb_str_append(&msg, " '");
  ccb_str_append(&msg, detail);
  ccb_str_append(&msg, "'");
  ccb_graph_diag(diags, code, msg.buf);
  ccb_str_free(&msg);
}

// Unknown dependency references. Returns the error count.
static int ccb_check_dep_refs(ccb_graph *g, ccb_diag_list *diags) {
  int errors = 0;
  for (size_t i = 0; i < g->count; i++) {
    ccb_task *t = g->tasks[i];
    for (size_t j = 0; j < t->deps_count; j++) {
      if (ccb_graph_find(g, t->deps[j]) != NULL) {
        continue;
      }
      ccb_vdiag_task(diags, "CCB-E1013", t->id,
        "depends on unknown task", t->deps[j]);
      errors++;
    }
  }
  return errors;
}

// Duplicate outputs and GENERATED inputs with no producer.
// Returns the error count.
static int ccb_check_outputs(ccb_graph *g, ccb_diag_list *diags) {
  int errors = 0;
  for (size_t i = 0; i < g->count; i++) {
    ccb_task *t = g->tasks[i];
    for (size_t j = 0; j < t->outputs_count; j++) {
      // Only compare against later tasks: each pair reports once.
      for (size_t k = i + 1; k < g->count; k++) {
        ccb_task *o = g->tasks[k];
        if (o == t) {
          continue;
        }
        for (size_t m = 0; m < o->outputs_count; m++) {
          if (strcmp(o->outputs[m], t->outputs[j]) != 0) {
            continue;
          }
          ccb_str msg;
          ccb_str_init(&msg);
          ccb_str_append(&msg, "tasks '");
          ccb_str_append(&msg, t->id);
          ccb_str_append(&msg, "' and '");
          ccb_str_append(&msg, o->id);
          ccb_str_append(&msg, "' both claim output '");
          ccb_str_append(&msg, t->outputs[j]);
          ccb_str_append(&msg, "'");
          ccb_graph_diag(diags, "CCB-E1013", msg.buf);
          ccb_str_free(&msg);
          errors++;
        }
      }
    }
    for (size_t j = 0; j < t->inputs_count; j++) {
      if (t->inputs[j].kind != CCB_TASK_INPUT_GENERATED) {
        continue;
      }
      if (ccb_has_producer(g, t, t->inputs[j].name)) {
        continue;
      }
      ccb_vdiag_task(diags, "CCB-E1013", t->id,
        "has generated input produced by no task",
        t->inputs[j].name);
      errors++;
    }
  }
  return errors;
}

// Depth-first cycle search. Colors: 0 unvisited, 1 on the stack,
// 2 finished. On a back edge, cycle_at is the stack position where
// the cycle starts and cycle_close is the task that closes it.
typedef struct {
  ccb_graph *g;
  unsigned char *color;
  size_t *stack;
  size_t nstack;
  size_t cycle_at;
  size_t cycle_len; // stack depth when the back edge was found
  size_t cycle_close;
  int found;
} ccb_cycle_ctx;

static void ccb_cycle_record(ccb_cycle_ctx *c, size_t dep_idx) {
  for (size_t k = 0; k < c->nstack; k++) {
    if (c->stack[k] == dep_idx) {
      c->cycle_at = k;
      break;
    }
  }
  c->cycle_len = c->nstack;
  c->cycle_close = dep_idx;
  c->found = 1;
}

static void ccb_dfs_visit(ccb_cycle_ctx *c, size_t idx) {
  if (c->found) {
    return;
  }
  c->color[idx] = 1;
  c->stack[c->nstack++] = idx;
  ccb_task *t = c->g->tasks[idx];
  for (size_t i = 0; i < t->deps_count && !c->found; i++) {
    size_t di = ccb_graph_index(c->g, t->deps[i]);
    if (di == (size_t)-1) {
      continue;
    }
    if (c->color[di] == 1) {
      ccb_cycle_record(c, di);
    } else if (c->color[di] == 0) {
      ccb_dfs_visit(c, di);
    }
  }
  c->nstack--;
  c->color[idx] = 2;
}

// Returns 1 when a cycle was found and reported, else 0.
static int ccb_check_cycle(ccb_graph *g, ccb_diag_list *diags) {
  size_t n = g->count;
  ccb_cycle_ctx c;
  memset(&c, 0, sizeof(c));
  c.g = g;
  c.color = ccb_xcalloc(n == 0 ? 1 : n, 1);
  c.stack = ccb_xmalloc((n == 0 ? 1 : n) * sizeof(*c.stack));
  for (size_t i = 0; i < n && !c.found; i++) {
    if (c.color[i] == 0) {
      ccb_dfs_visit(&c, i);
    }
  }
  int found = c.found;
  if (found) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "task dependency cycle: ");
    for (size_t k = c.cycle_at; k < c.cycle_len; k++) {
      if (k > c.cycle_at) {
        ccb_str_append(&msg, " -> ");
      }
      ccb_str_append(&msg, g->tasks[c.stack[k]]->id);
    }
    ccb_str_append(&msg, " -> ");
    ccb_str_append(&msg, g->tasks[c.cycle_close]->id);
    ccb_graph_diag(diags, "CCB-E1014", msg.buf);
    ccb_str_free(&msg);
  }
  free(c.color);
  free(c.stack);
  return found;
}

ccb_status ccb_graph_validate(ccb_graph *g, ccb_diag_list *diags) {
  int errors = 0;
  for (size_t i = 0; i < g->count; i++) {
    for (size_t j = 0; j < i; j++) {
      if (strcmp(g->tasks[i]->id, g->tasks[j]->id) != 0) {
        continue;
      }
      ccb_str msg;
      ccb_str_init(&msg);
      ccb_str_append(&msg, "duplicate task id '");
      ccb_str_append(&msg, g->tasks[i]->id);
      ccb_str_append(&msg, "'");
      ccb_graph_diag(diags, "CCB-E1012", msg.buf);
      ccb_str_free(&msg);
      errors++;
    }
  }
  errors += ccb_check_dep_refs(g, diags);
  errors += ccb_check_outputs(g, diags);
  if (errors == 0) {
    errors += ccb_check_cycle(g, diags);
  }
  return errors == 0 ? CCB_OK : CCB_ERR_CONFIG;
}

ccb_status ccb_graph_check_source_inputs(ccb_graph *g,
    ccb_diag_list *diags) {
  int errors = 0;
  for (size_t i = 0; i < g->count; i++) {
    ccb_task *t = g->tasks[i];
    for (size_t j = 0; j < t->inputs_count; j++) {
      if (t->inputs[j].kind != CCB_TASK_INPUT_SOURCE) {
        continue;
      }
      if (access(t->inputs[j].name, F_OK) == 0) {
        continue;
      }
      ccb_vdiag_task(diags, "CCB-E1015", t->id,
        "has missing source input", t->inputs[j].name);
      errors++;
    }
  }
  return errors == 0 ? CCB_OK : CCB_ERR_CONFIG;
}

// ---------- planning ----------

// Decrement the remaining-dependency count of every task that
// depends on tasks[done_idx].
static void ccb_plan_decrement(ccb_graph *g, size_t *remaining,
    size_t done_idx) {
  for (size_t j = 0; j < g->count; j++) {
    ccb_task *t = g->tasks[j];
    for (size_t k = 0; k < t->deps_count; k++) {
      if (strcmp(t->deps[k], g->tasks[done_idx]->id) == 0 &&
          remaining[j] > 0) {
        remaining[j]--;
      }
    }
  }
}

ccb_status ccb_graph_plan(ccb_graph *g, ccb_task ***order_out,
    size_t *count_out) {
  size_t n = g->count;
  ccb_task **order = ccb_xmalloc((n == 0 ? 1 : n) * sizeof(*order));
  size_t *remaining = ccb_xmalloc((n == 0 ? 1 : n) * sizeof(*remaining));
  unsigned char *placed = ccb_xcalloc(n == 0 ? 1 : n, 1);
  for (size_t i = 0; i < n; i++) {
    remaining[i] = g->tasks[i]->deps_count;
  }
  size_t nplaced = 0;
  while (nplaced < n) {
    int progress = 0;
    for (size_t i = 0; i < n; i++) {
      if (placed[i] || remaining[i] > 0) {
        continue;
      }
      order[nplaced++] = g->tasks[i];
      placed[i] = 1;
      progress = 1;
      ccb_plan_decrement(g, remaining, i);
    }
    if (!progress) {
      free(order);
      free(remaining);
      free(placed);
      return CCB_ERR_CONFIG;
    }
  }
  free(remaining);
  free(placed);
  *order_out = order;
  *count_out = n;
  return CCB_OK;
}

static int ccb_all_deps_done(ccb_graph *g, ccb_task *t) {
  for (size_t i = 0; i < t->deps_count; i++) {
    ccb_task *d = ccb_graph_find(g, t->deps[i]);
    if (d == NULL || d->state != CCB_TASK_DONE) {
      return 0;
    }
  }
  return 1;
}

size_t ccb_graph_mark_ready(ccb_graph *g) {
  size_t n = 0;
  for (size_t i = 0; i < g->count; i++) {
    ccb_task *t = g->tasks[i];
    if (t->state != CCB_TASK_WAITING) {
      continue;
    }
    if (!ccb_all_deps_done(g, t)) {
      continue;
    }
    t->state = CCB_TASK_READY;
    n++;
  }
  return n;
}

static ccb_task *ccb_first_ready(ccb_graph *g) {
  for (size_t i = 0; i < g->count; i++) {
    if (g->tasks[i]->state == CCB_TASK_READY) {
      return g->tasks[i];
    }
  }
  return NULL;
}

// Tag every diagnostic added during a failed task with that task's ID.
static void ccb_stamp_diags(ccb_diag_list *diags, size_t from,
    const char *task_id) {
  for (size_t i = from; i < diags->count; i++) {
    ccb_diag *d = &diags->items[i];
    if (d->task_id != NULL) {
      continue;
    }
    d->task_id = ccb_xstrdup(task_id);
  }
}

ccb_status ccb_graph_execute(ccb_graph *g, void *ctx, ccb_log *log,
    ccb_diag_list *diags) {
  if (ccb_graph_validate(g, diags) != CCB_OK) {
    return CCB_ERR_CONFIG;
  }
  while (1) {
    ccb_graph_mark_ready(g);
    ccb_task *t = ccb_first_ready(g);
    if (t == NULL) {
      break;
    }
    t->state = CCB_TASK_RUNNING;
    ccb_log_task_start(log, t->id);
    long t0 = ccb_graph_now_ms();
    size_t diag_before = diags != NULL ? diags->count : 0;
    ccb_status rc = t->run != NULL ? t->run(ctx, t) : CCB_OK;
    t->duration_ms = ccb_graph_now_ms() - t0;
    if (rc == CCB_OK) {
      t->state = CCB_TASK_DONE;
      ccb_log_task_done(log, t->id, t->duration_ms, t->skipped);
      continue;
    }
    t->state = CCB_TASK_FAILED;
    ccb_log_task_failed(log, t->id, t->duration_ms);
    if (diags != NULL) {
      ccb_stamp_diags(diags, diag_before, t->id);
    }
    for (size_t i = 0; i < g->count; i++) {
      ccb_task *rest = g->tasks[i];
      if (rest->state == CCB_TASK_WAITING ||
          rest->state == CCB_TASK_READY) {
        rest->state = CCB_TASK_CANCELLED;
      }
    }
    return rc;
  }
  for (size_t i = 0; i < g->count; i++) {
    if (g->tasks[i]->state == CCB_TASK_WAITING) {
      return CCB_ERR_CONFIG;
    }
  }
  return CCB_OK;
}

// ---------- affected tasks ----------

ccb_status ccb_graph_affected(ccb_graph *g, const char **changed,
    size_t n_changed, char ***out_ids, size_t *out_count) {
  ccb_task **order = NULL;
  size_t norder = 0;
  if (ccb_graph_plan(g, &order, &norder) != CCB_OK) {
    return CCB_ERR_CONFIG;
  }
  // Affected flags indexed by task position in g->tasks. Plan order
  // guarantees a dependency's flag is set before its dependents.
  unsigned char *aff = ccb_xcalloc(g->count == 0 ? 1 : g->count, 1);
  for (size_t k = 0; k < norder; k++) {
    ccb_task *t = order[k];
    size_t ti = ccb_graph_index(g, t->id);
    int hit = 0;
    for (size_t i = 0; i < t->inputs_count && !hit; i++) {
      for (size_t j = 0; j < n_changed; j++) {
        if (strcmp(t->inputs[i].name, changed[j]) == 0) {
          hit = 1;
          break;
        }
      }
    }
    if (!hit) {
      for (size_t i = 0; i < t->deps_count && !hit; i++) {
        size_t di = ccb_graph_index(g, t->deps[i]);
        if (di != (size_t)-1 && aff[di]) {
          hit = 1;
        }
      }
    }
    aff[ti] = hit ? 1 : 0;
  }
  char **ids = NULL;
  size_t nids = 0;
  size_t cap = 0;
  for (size_t k = 0; k < norder; k++) {
    size_t ti = ccb_graph_index(g, order[k]->id);
    if (!aff[ti]) {
      continue;
    }
    if (nids == cap) {
      size_t ncap = cap == 0 ? 8 : cap * 2;
      ids = ccb_xrealloc(ids, ncap * sizeof(*ids));
      cap = ncap;
    }
    ids[nids++] = ccb_xstrdup(order[k]->id);
  }
  free(aff);
  free(order);
  *out_ids = ids;
  *out_count = nids;
  return CCB_OK;
}

// ---------- summary + plan output ----------

void ccb_graph_summary_json(ccb_graph *g, ccb_str *out) {
  ccb_task **order = NULL;
  size_t norder = 0;
  int planned = ccb_graph_plan(g, &order, &norder) == CCB_OK;
  ccb_str_append(out, "[");
  size_t n = planned ? norder : g->count;
  for (size_t k = 0; k < n; k++) {
    ccb_task *t = planned ? order[k] : g->tasks[k];
    if (k > 0) {
      ccb_str_append(out, ",");
    }
    ccb_str_append(out, "{\"id\":");
    ccb_json_escape(out, t->id);
    ccb_str_append(out, ",\"state\":");
    ccb_json_escape(out, ccb_task_state_name(t->state));
    ccb_str_appendf(out, ",\"duration_ms\":%ld,\"skipped\":%s}",
      t->duration_ms, t->skipped ? "true" : "false");
  }
  ccb_str_append(out, "]");
  free(order);
}

ccb_status ccb_graph_emit_plan_human(ccb_graph *g, FILE *f) {
  ccb_task **order = NULL;
  size_t norder = 0;
  if (ccb_graph_plan(g, &order, &norder) != CCB_OK) {
    return CCB_ERR_CONFIG;
  }
  fprintf(f, "build plan (%zu tasks, in execution order):\n", norder);
  for (size_t k = 0; k < norder; k++) {
    ccb_task *t = order[k];
    if (t->deps_count == 0) {
      fprintf(f, "  %s\n", t->id);
      continue;
    }
    fprintf(f, "  %s (after: ", t->id);
    for (size_t j = 0; j < t->deps_count; j++) {
      if (j > 0) {
        fprintf(f, ", ");
      }
      fprintf(f, "%s", t->deps[j]);
    }
    fprintf(f, ")\n");
  }
  free(order);
  return CCB_OK;
}

ccb_status ccb_graph_emit_plan_json(ccb_graph *g, FILE *f) {
  ccb_task **order = NULL;
  size_t norder = 0;
  if (ccb_graph_plan(g, &order, &norder) != CCB_OK) {
    return CCB_ERR_CONFIG;
  }
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"event\":\"build_plan\",\"tasks\":[");
  for (size_t k = 0; k < norder; k++) {
    ccb_task *t = order[k];
    if (k > 0) {
      ccb_str_append(&out, ",");
    }
    ccb_str_append(&out, "{\"id\":");
    ccb_json_escape(&out, t->id);
    ccb_str_append(&out, ",\"type\":");
    ccb_json_escape(&out, t->type);
    ccb_str_append(&out, ",\"deps\":[");
    for (size_t j = 0; j < t->deps_count; j++) {
      if (j > 0) {
        ccb_str_append(&out, ",");
      }
      ccb_json_escape(&out, t->deps[j]);
    }
    ccb_str_append(&out, "],\"inputs\":[");
    for (size_t j = 0; j < t->inputs_count; j++) {
      if (j > 0) {
        ccb_str_append(&out, ",");
      }
      ccb_json_escape(&out, t->inputs[j].name);
    }
    ccb_str_append(&out, "],\"outputs\":[");
    for (size_t j = 0; j < t->outputs_count; j++) {
      if (j > 0) {
        ccb_str_append(&out, ",");
      }
      ccb_json_escape(&out, t->outputs[j]);
    }
    ccb_str_append(&out, "]}");
  }
  ccb_str_append(&out, "]}\n");
  fputs(out.buf != NULL ? out.buf : "", f);
  ccb_str_free(&out);
  free(order);
  return CCB_OK;
}
