// test_graph: unit tests for the task graph engine (ccb_graph) and
// the pipeline graph builder (ccb_pipeline_graph). Fast: no real
// Android build. The pipeline-equivalence proof at unit level is
// the plan-order test: the graph's topological order must equal the
// legacy stage order exactly (stages 0–14); byte-identical APKs are
// verified by a real hello build before/after (see docs/graph.md).
#include "test.h"
#include "ccb_graph.h"
#include "ccb_pipeline_graph.h"
#include "ccb_config.h"
#include "ccb_diag.h"
#include "ccb_log.h"

static char BASE[256];

// ---------- run-function recorder ----------

static char g_seen[512];
static size_t g_nseen;

static void record_id(const char *id) {
  size_t len = strlen(id);
  if (g_nseen + len + 1 < sizeof(g_seen)) {
    memcpy(g_seen + g_nseen, id, len);
    g_nseen += len;
    g_seen[g_nseen++] = ' ';
    g_seen[g_nseen] = '\0';
  }
}

static ccb_status record_run(void *ctx, ccb_task *t) {
  CCB_UNUSED(ctx);
  record_id(t->id);
  return CCB_OK;
}

static ccb_status record_skip_run(void *ctx, ccb_task *t) {
  CCB_UNUSED(ctx);
  record_id(t->id);
  t->skipped = 1;
  return CCB_OK;
}

// Skips with a logged cache reason: ctx is the ccb_log, so the
// reason flows through the log stash to the task's skip_reason.
static ccb_status record_skip_reason_run(void *ctx, ccb_task *t) {
  ccb_log *log = ctx;
  record_id(t->id);
  ccb_log_cache(log, t->id, 1, "inputs unchanged");
  t->skipped = 1;
  return CCB_OK;
}

// True when t declares an input with this name and kind.
static int task_has_input(ccb_task *t, const char *name,
    ccb_task_input_kind kind) {
  for (size_t i = 0; i < t->inputs_count; i++) {
    if (strcmp(t->inputs[i].name, name) == 0 &&
        t->inputs[i].kind == kind) {
      return 1;
    }
  }
  return 0;
}

// Fails, pushing one diagnostic into the ctx diag list so the
// executor's task-ID stamping can be checked.
static ccb_status fail_run(void *ctx, ccb_task *t) {
  ccb_diag_list *diags = ctx;
  record_id(t->id);
  ccb_diag d = ccb_diag_make("CCB-E3004", "kotlinc", "boom");
  ccb_diag_list_push(diags, &d);
  return CCB_ERR_BUILD;
}

static void reset_seen(void) {
  g_nseen = 0;
  g_seen[0] = '\0';
}

// ---------- helpers ----------

static void check_plan_order(ccb_graph *g, const char **want,
    size_t n) {
  ccb_task **order = NULL;
  size_t norder = 0;
  CHECK(ccb_graph_plan(g, &order, &norder) == CCB_OK);
  CHECK(norder == n);
  if (norder == n) {
    for (size_t i = 0; i < n; i++) {
      CHECK_STR_EQ(order[i]->id, want[i]);
    }
  }
  free(order);
}

static int diag_has_code(ccb_diag_list *diags, const char *code) {
  for (size_t i = 0; i < diags->count; i++) {
    if (strcmp(diags->items[i].code, code) == 0) {
      return 1;
    }
  }
  return 0;
}

static const char *diag_message(ccb_diag_list *diags, size_t i) {
  if (i >= diags->count) {
    return "";
  }
  return diags->items[i].message != NULL ?
    diags->items[i].message : "";
}

// Plan order as a space-separated string of task IDs.
static void plan_string(ccb_graph *g, char *buf, size_t cap) {
  ccb_task **order = NULL;
  size_t norder = 0;
  buf[0] = '\0';
  if (ccb_graph_plan(g, &order, &norder) != CCB_OK) {
    return;
  }
  size_t used = 0;
  for (size_t i = 0; i < norder; i++) {
    size_t len = strlen(order[i]->id);
    if (used + len + 1 < cap) {
      memcpy(buf + used, order[i]->id, len);
      used += len;
      buf[used++] = ' ';
      buf[used] = '\0';
    }
  }
  free(order);
}

// Position of task t in the plan order, or norder when absent.
static size_t plan_pos(ccb_task **order, size_t norder, ccb_task *t) {
  for (size_t k = 0; k < norder; k++) {
    if (order[k] == t) {
      return k;
    }
  }
  return norder;
}

// Every dependency appears before its dependent in the plan.
static void check_plan_valid(ccb_graph *g) {
  ccb_task **order = NULL;
  size_t norder = 0;
  CHECK(ccb_graph_plan(g, &order, &norder) == CCB_OK);
  CHECK(norder == g->count);
  for (size_t i = 0; i < g->count; i++) {
    ccb_task *t = g->tasks[i];
    size_t tpos = plan_pos(order, norder, t);
    for (size_t j = 0; j < t->deps_count; j++) {
      ccb_task *d = ccb_graph_find(g, t->deps[j]);
      CHECK(plan_pos(order, norder, d) < tpos);
    }
  }
  free(order);
}

// Layered synthetic graph: 10 chains of 5 tasks, joined by one task
// depending on all 10 chain ends (51 tasks). Each link consumes the
// previous task's output, like a real build.
static ccb_graph *layered_graph(void) {
  ccb_graph *g = ccb_graph_create();
  char id[16];
  char out[16];
  char prev[16];
  for (size_t c = 0; c < 10; c++) {
    prev[0] = '\0';
    for (size_t r = 0; r < 5; r++) {
      snprintf(id, sizeof id, "c%zu_%zu", r, c);
      ccb_task *t = ccb_graph_add_task(g, id, "test",
        CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
      snprintf(out, sizeof out, "out-%zu-%zu", r, c);
      ccb_task_add_output(t, out);
      if (r > 0) {
        ccb_task_add_dep(t, prev);
        snprintf(out, sizeof out, "out-%zu-%zu", r - 1, c);
        ccb_task_add_input(t, out, CCB_TASK_INPUT_GENERATED);
      }
      snprintf(prev, sizeof prev, "%s", id);
    }
  }
  ccb_task *join = ccb_graph_add_task(g, "join", "test",
    CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
  for (size_t c = 0; c < 10; c++) {
    snprintf(prev, sizeof prev, "c4_%zu", c);
    ccb_task_add_dep(join, prev);
    snprintf(out, sizeof out, "out-4-%zu", c);
    ccb_task_add_input(join, out, CCB_TASK_INPUT_GENERATED);
  }
  return g;
}

// Diamond graph: a -> {b, c} -> d. b consumes "src-b", c consumes
// "src-c", d consumes b's output "out-b". run is used for every task
// except b, which uses b_run (so the failure test can fail only b).
static ccb_graph *diamond_graph2(ccb_task_run_fn run,
    ccb_task_run_fn b_run) {
  ccb_graph *g = ccb_graph_create();
  ccb_task *a = ccb_graph_add_task(g, "a", "test",
    CCB_TASK_CACHE_FINGERPRINT, run, NULL);
  ccb_task *b = ccb_graph_add_task(g, "b", "test",
    CCB_TASK_CACHE_FINGERPRINT, b_run, NULL);
  ccb_task *c = ccb_graph_add_task(g, "c", "test",
    CCB_TASK_CACHE_FINGERPRINT, run, NULL);
  ccb_task *d = ccb_graph_add_task(g, "d", "test",
    CCB_TASK_CACHE_FINGERPRINT, run, NULL);
  ccb_task_add_input(b, "src-b", CCB_TASK_INPUT_SOURCE);
  ccb_task_add_output(b, "out-b");
  ccb_task_add_input(c, "src-c", CCB_TASK_INPUT_SOURCE);
  ccb_task_add_input(d, "out-b", CCB_TASK_INPUT_GENERATED);
  ccb_task_add_dep(b, "a");
  ccb_task_add_dep(c, "a");
  ccb_task_add_dep(d, "b");
  ccb_task_add_dep(d, "c");
  CCB_UNUSED(a);
  return g;
}

static ccb_graph *diamond_graph(ccb_task_run_fn run) {
  return diamond_graph2(run, run);
}

static ccb_config *fake_config(int is_library) {
  ccb_config *c = ccb_xcalloc(1, sizeof(*c));
  c->is_library = is_library;
  c->manifest = ccb_xstrdup("src/main/AndroidManifest.xml");
  c->res = ccb_xstrdup("src/main/res");
  c->kotlin_dir = ccb_xstrdup("src/main/kotlin");
  c->java_dir = ccb_xstrdup("src/main/java");
  return c;
}

int main(void) {
  ccb_test_tmpdir(BASE, sizeof BASE, "graph");
  ccb_diag_list diags;
  ccb_diag_list_init(&diags);
  ccb_log log;
  ccb_log_init(&log, 0, 0);

  // --- task creation defaults ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "compile", "kotlin",
      CCB_TASK_CACHE_FINGERPRINT, record_run, &diags);
    CHECK(t != NULL);
    CHECK(t->state == CCB_TASK_WAITING);
    CHECK(t->duration_ms == -1);
    CHECK(t->skipped == 0);
    ccb_task_add_input(t, "src.kt", CCB_TASK_INPUT_SOURCE);
    ccb_task_add_output(t, "classes");
    ccb_task_add_dep(t, "link");
    CHECK(t->inputs_count == 1);
    CHECK(t->outputs_count == 1);
    CHECK(t->deps_count == 1);
    CHECK_STR_EQ(ccb_task_state_name(CCB_TASK_DONE), "done");
    CHECK_STR_EQ(ccb_task_state_name(CCB_TASK_CANCELLED),
      "cancelled");
    ccb_graph_free(g);
  }

  // --- duplicate task ID ---
  {
    ccb_graph *g = ccb_graph_create();
    CHECK(ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, &diags) != NULL);
    size_t before = diags.count;
    CHECK(ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, &diags) == NULL);
    CHECK(diags.count == before + 1);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1012");
    ccb_graph_free(g);
  }

  // --- unknown dependency ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_dep(t, "ghost");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK(diag_has_code(&diags, "CCB-E1013"));
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "unknown task"));
    ccb_graph_free(g);
  }

  // --- self-cycle names the path ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_dep(t, "a");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1014");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "a -> a"));
    ccb_graph_free(g);
  }

  // --- multi-node cycle names the path ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *c = ccb_graph_add_task(g, "c", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_dep(a, "c");
    ccb_task_add_dep(b, "a");
    ccb_task_add_dep(c, "b");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1014");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "a -> c -> b -> a"));
    ccb_graph_free(g);
  }

  // --- generated input with no producer ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "x", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_input(t, "ghost-out",
      CCB_TASK_INPUT_GENERATED);
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK(diag_has_code(&diags, "CCB-E1013"));
    ccb_graph_free(g);
  }

  // --- duplicate output, reported once ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_output(a, "out");
    ccb_task_add_output(b, "out");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK(diags.count == before + 1);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
    ccb_graph_free(g);
  }

  // --- source input existence check ---
  {
    char *real = ccb_test_path(BASE, "exists.txt");
    CHECK(ccb_write_file(real, "x", 1) == CCB_OK);
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_input(t, real, CCB_TASK_INPUT_SOURCE);
    CHECK(ccb_graph_check_source_inputs(g, &diags) == CCB_OK);
    char *missing = ccb_test_path(BASE, "missing.txt");
    ccb_task_add_input(t, missing, CCB_TASK_INPUT_SOURCE);
    size_t before = diags.count;
    CHECK(ccb_graph_check_source_inputs(g, &diags) ==
      CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1015");
    ccb_graph_free(g);
  }

  // --- ENV inputs need no producer; optional SOURCE inputs may
  // be missing; required inputs default on ---
  {
    char *missing = ccb_test_path(BASE, "optional-missing.txt");
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_input(t, "MY_SECRET_PW", CCB_TASK_INPUT_ENV);
    ccb_task_add_input_opt(t, missing, CCB_TASK_INPUT_SOURCE, 0);
    CHECK(t->inputs[0].required == 1);
    CHECK(t->inputs[1].required == 0);
    CHECK_STR_EQ(ccb_task_input_kind_name(CCB_TASK_INPUT_ENV),
      "env");
    CHECK(ccb_graph_validate(g, &diags) == CCB_OK);
    CHECK(ccb_graph_check_source_inputs(g, &diags) == CCB_OK);
    CHECK(ccb_graph_has_input(g, "MY_SECRET_PW"));
    CHECK(!ccb_graph_has_input(g, "nope"));
    ccb_graph_free(g);
  }

  // --- topological order on a diamond ---
  {
    ccb_graph *g = diamond_graph(record_run);
    const char *want[] = { "a", "b", "c", "d" };
    check_plan_order(g, want, 4);
    ccb_graph_free(g);
  }

  // --- ready-task identification ---
  {
    ccb_graph *g = diamond_graph(record_run);
    CHECK(ccb_graph_mark_ready(g) == 1);
    ccb_task *a = ccb_graph_find(g, "a");
    a->state = CCB_TASK_DONE;
    CHECK(ccb_graph_mark_ready(g) == 2);
    CHECK(ccb_graph_find(g, "b")->state == CCB_TASK_READY);
    CHECK(ccb_graph_find(g, "c")->state == CCB_TASK_READY);
    CHECK(ccb_graph_find(g, "d")->state == CCB_TASK_WAITING);
    ccb_graph_free(g);
  }

  // --- affected-task calculation ---
  {
    ccb_graph *g = diamond_graph(record_run);
    char **ids = NULL;
    size_t nids = 0;
    const char *changed_b[] = { "src-b" };
    CHECK(ccb_graph_affected(g, changed_b, 1, &ids, &nids) ==
      CCB_OK);
    CHECK(nids == 2);
    if (nids == 2) {
      CHECK_STR_EQ(ids[0], "b");
      CHECK_STR_EQ(ids[1], "d");
    }
    ccb_free_str_array(ids, nids);
    const char *changed_c[] = { "src-c" };
    CHECK(ccb_graph_affected(g, changed_c, 1, &ids, &nids) ==
      CCB_OK);
    CHECK(nids == 2);
    if (nids == 2) {
      CHECK_STR_EQ(ids[0], "c");
      CHECK_STR_EQ(ids[1], "d");
    }
    ccb_free_str_array(ids, nids);
    const char *changed_none[] = { "unrelated" };
    CHECK(ccb_graph_affected(g, changed_none, 1, &ids, &nids) ==
      CCB_OK);
    CHECK(nids == 0);
    ccb_free_str_array(ids, nids);
    ccb_graph_free(g);
  }

  // --- executor: order, states, timings, skipped flag ---
  {
    reset_seen();
    ccb_graph *g = diamond_graph(record_skip_run);
    // Every task reports a cache-hit skip here, proving the flag
    // flows from the run function to the task and the summary.
    ccb_diag_list exec_diags;
    ccb_diag_list_init(&exec_diags);
    CHECK(ccb_graph_execute(g, NULL, &log, &exec_diags) == CCB_OK);
    CHECK_STR_EQ(g_seen, "a b c d ");
    for (size_t i = 0; i < g->count; i++) {
      CHECK(g->tasks[i]->state == CCB_TASK_DONE);
      CHECK(g->tasks[i]->duration_ms >= 0);
      CHECK(g->tasks[i]->skipped == 1);
    }
    ccb_str sum;
    ccb_str_init(&sum);
    ccb_graph_summary_json(g, &sum);
    CHECK(ccb_test_contains(sum.buf, "\"id\":\"a\""));
    CHECK(ccb_test_contains(sum.buf, "\"state\":\"done\""));
    CHECK(ccb_test_contains(sum.buf, "\"skipped\":true"));
    ccb_str_free(&sum);
    ccb_diag_list_free(&exec_diags);
    ccb_graph_free(g);
  }

  // --- executor: skip reason flows from the run's cache log to
  // the task and the summary; a skip with no logged reason has
  // none, and a reason never leaks across tasks ---
  {
    reset_seen();
    ccb_graph *g = diamond_graph(record_skip_reason_run);
    ccb_diag_list exec_diags;
    ccb_diag_list_init(&exec_diags);
    CHECK(ccb_graph_execute(g, &log, &log, &exec_diags) ==
      CCB_OK);
    for (size_t i = 0; i < g->count; i++) {
      CHECK(g->tasks[i]->skipped == 1);
      CHECK(g->tasks[i]->skip_reason != NULL);
      if (g->tasks[i]->skip_reason != NULL) {
        CHECK_STR_EQ(g->tasks[i]->skip_reason,
          "inputs unchanged");
      }
    }
    ccb_str sum;
    ccb_str_init(&sum);
    ccb_graph_summary_json(g, &sum);
    CHECK(ccb_test_contains(sum.buf,
      "\"skip_reason\":\"inputs unchanged\""));
    ccb_str_free(&sum);
    ccb_diag_list_free(&exec_diags);
    ccb_graph_free(g);
    reset_seen();
    g = diamond_graph(record_skip_run);
    ccb_diag_list_init(&exec_diags);
    CHECK(ccb_graph_execute(g, NULL, &log, &exec_diags) ==
      CCB_OK);
    for (size_t i = 0; i < g->count; i++) {
      CHECK(g->tasks[i]->skipped == 1);
      CHECK(g->tasks[i]->skip_reason == NULL);
    }
    ccb_str_init(&sum);
    ccb_graph_summary_json(g, &sum);
    CHECK(ccb_test_contains(sum.buf, "\"skip_reason\":null"));
    ccb_str_free(&sum);
    ccb_diag_list_free(&exec_diags);
    ccb_graph_free(g);
  }

  // --- executor failure: fail-fast, cancel rest, stamp task ID ---
  {
    reset_seen();
    ccb_graph *g = diamond_graph2(record_run, fail_run);
    // b fails; c and d must be cancelled, never run.
    ccb_diag_list exec_diags;
    ccb_diag_list_init(&exec_diags);
    ccb_status rc = ccb_graph_execute(g, &exec_diags, &log,
      &exec_diags);
    CHECK(rc == CCB_ERR_BUILD);
    CHECK_STR_EQ(g_seen, "a b ");
    CHECK(ccb_graph_find(g, "a")->state == CCB_TASK_DONE);
    CHECK(ccb_graph_find(g, "b")->state == CCB_TASK_FAILED);
    CHECK(ccb_graph_find(g, "c")->state == CCB_TASK_CANCELLED);
    CHECK(ccb_graph_find(g, "d")->state == CCB_TASK_CANCELLED);
    CHECK(exec_diags.count == 1);
    if (exec_diags.count == 1) {
      CHECK(exec_diags.items[0].task_id != NULL);
      CHECK_STR_EQ(exec_diags.items[0].task_id, "b");
    }
    ccb_diag_list_free(&exec_diags);
    ccb_graph_free(g);
  }

  // --- pipeline graph: app plan order equals the legacy stage order ---
  {
    ccb_config *cfg = fake_config(0);
    ccb_diag_list gdiags;
    ccb_diag_list_init(&gdiags);
    ccb_graph *g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    CHECK(gdiags.count == 0);
    const char *want[] = { "config", "deps", "libraries", "modules",
      "merge-manifest", "compile-res", "link", "compile-code", "dex",
      "dexcheck", "native", "package", "align", "sign", "verify" };
    if (g != NULL) {
      check_plan_order(g, want, 15);
      CHECK_STR_EQ(ccb_graph_find(g, "dexcheck")->type, "dexcheck");
      CHECK(ccb_graph_find(g, "dexcheck")->cache ==
        CCB_TASK_CACHE_ALWAYS_RUN);
      CHECK(ccb_graph_find(g, "config")->run == NULL);
      // The config task declares its real inputs and both of its
      // outputs; every task consumes the toolchain it resolves.
      ccb_task *config = ccb_graph_find(g, "config");
      CHECK(config != NULL);
      if (config != NULL) {
        CHECK(task_has_input(config, "ccb.toml",
          CCB_TASK_INPUT_SOURCE));
        CHECK(task_has_input(config,
          "src/main/AndroidManifest.xml", CCB_TASK_INPUT_SOURCE));
        CHECK(config->outputs_count == 2);
      }
      for (size_t i = 0; i < g->count; i++) {
        ccb_task *t = g->tasks[i];
        if (strcmp(t->id, "config") == 0) {
          continue;
        }
        CHECK(task_has_input(t, "project-model",
          CCB_TASK_INPUT_GENERATED));
        CHECK(task_has_input(t, "toolchain",
          CCB_TASK_INPUT_GENERATED));
      }
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
  }

  // --- pipeline graph: AAB drops align, renames package ---
  {
    ccb_config *cfg = fake_config(0);
    ccb_diag_list gdiags;
    ccb_diag_list_init(&gdiags);
    ccb_graph *g = ccb_pipeline_graph_build(cfg, 1, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      const char *want[] = { "config", "deps", "libraries",
        "modules", "merge-manifest", "compile-res", "link",
        "compile-code", "dex", "dexcheck", "native", "package-aab",
        "sign", "verify" };
      check_plan_order(g, want, 14);
      CHECK(ccb_graph_find(g, "align") == NULL);
      CHECK(ccb_graph_find(g, "package") == NULL);
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
  }

  // --- pipeline graph: library projects stop after deps ---
  {
    ccb_config *cfg = fake_config(1);
    ccb_diag_list gdiags;
    ccb_diag_list_init(&gdiags);
    ccb_graph *g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      const char *want[] = { "config", "deps", "library" };
      check_plan_order(g, want, 3);
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
  }

  // --- empty graph: validates, plans to nothing, runs, summarizes ---
  {
    ccb_graph *g = ccb_graph_create();
    CHECK(ccb_graph_validate(g, &diags) == CCB_OK);
    ccb_task **order = NULL;
    size_t norder = 99;
    CHECK(ccb_graph_plan(g, &order, &norder) == CCB_OK);
    CHECK(norder == 0);
    free(order);
    ccb_diag_list exec_diags;
    ccb_diag_list_init(&exec_diags);
    CHECK(ccb_graph_execute(g, NULL, &log, &exec_diags) == CCB_OK);
    ccb_diag_list_free(&exec_diags);
    ccb_str sum;
    ccb_str_init(&sum);
    ccb_graph_summary_json(g, &sum);
    CHECK_STR_EQ(sum.buf, "[]");
    ccb_str_free(&sum);
    char **ids = NULL;
    size_t nids = 99;
    const char *changed[] = { "x" };
    CHECK(ccb_graph_affected(g, changed, 1, &ids, &nids) ==
      CCB_OK);
    CHECK(nids == 0);
    ccb_free_str_array(ids, nids);
    ccb_graph_free(g);
  }

  // --- single-node graph ---
  {
    reset_seen();
    ccb_graph *g = ccb_graph_create();
    ccb_graph_add_task(g, "only", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    CHECK(ccb_graph_validate(g, &diags) == CCB_OK);
    const char *want[] = { "only" };
    check_plan_order(g, want, 1);
    CHECK(ccb_graph_mark_ready(g) == 1);
    ccb_diag_list exec_diags;
    ccb_diag_list_init(&exec_diags);
    CHECK(ccb_graph_execute(g, NULL, &log, &exec_diags) == CCB_OK);
    CHECK_STR_EQ(g_seen, "only ");
    ccb_diag_list_free(&exec_diags);
    ccb_graph_free(g);
  }

  // --- duplicate dependency declaration ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_dep(b, "a");
    ccb_task_add_dep(b, "a");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "more than once"));
    ccb_graph_free(g);
    CCB_UNUSED(a);
  }

  // --- duplicate input, same kind ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_input(t, "src", CCB_TASK_INPUT_SOURCE);
    ccb_task_add_input(t, "src", CCB_TASK_INPUT_SOURCE);
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "more than once"));
    ccb_graph_free(g);
  }

  // --- same input as both SOURCE and GENERATED ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_input(t, "x", CCB_TASK_INPUT_SOURCE);
    ccb_task_add_input(t, "x", CCB_TASK_INPUT_GENERATED);
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "both source and generated"));
    ccb_graph_free(g);
  }

  // --- duplicate output within one task ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_output(t, "out");
    ccb_task_add_output(t, "out");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "more than once"));
    ccb_graph_free(g);
  }

  // --- SOURCE input that is also the task's own output ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_input(t, "out", CCB_TASK_INPUT_SOURCE);
    ccb_task_add_output(t, "out");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "also its own output"));
    ccb_graph_free(g);
  }

  // --- GENERATED input produced by the task itself ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *t = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_input(t, "out", CCB_TASK_INPUT_GENERATED);
    ccb_task_add_output(t, "out");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
    CHECK(ccb_test_contains(diag_message(&diags, before),
      "produced by itself"));
    ccb_graph_free(g);
  }

  // --- GENERATED input whose producer is not a dependency ---
  // The consumer is added first on one pass and second on the other:
  // without this check the plan would run it before the producer
  // exists. The missing edge, not the insertion order, is the defect.
  {
    for (int flip = 0; flip < 2; flip++) {
      ccb_graph *g = ccb_graph_create();
      ccb_task *first = ccb_graph_add_task(g,
        flip == 0 ? "c" : "p", "test",
        CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
      ccb_task *second = ccb_graph_add_task(g,
        flip == 0 ? "p" : "c", "test",
        CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
      ccb_task *p = flip == 0 ? second : first;
      ccb_task *c = flip == 0 ? first : second;
      ccb_task_add_output(p, "out");
      ccb_task_add_input(c, "out", CCB_TASK_INPUT_GENERATED);
      size_t before = diags.count;
      CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
      CHECK_STR_EQ(diags.items[before].code, "CCB-E1013");
      CHECK(ccb_test_contains(diag_message(&diags, before),
        "does not depend on"));
      CHECK(ccb_test_contains(diag_message(&diags, before),
        "'p'"));
      ccb_graph_free(g);
    }
  }

  // --- GENERATED input whose producer is a transitive dependency ---
  // The pipeline shape: the producer is an ancestor, not a direct
  // dep (e.g. package consumes linked-apk from link). Valid.
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *p = ccb_graph_add_task(g, "p", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *m = ccb_graph_add_task(g, "m", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *c = ccb_graph_add_task(g, "c", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_output(p, "out");
    ccb_task_add_dep(m, "p");
    ccb_task_add_dep(c, "m");
    ccb_task_add_input(c, "out", CCB_TASK_INPUT_GENERATED);
    CHECK(ccb_graph_validate(g, &diags) == CCB_OK);
    ccb_graph_free(g);
  }

  // --- deterministic planning: same graph twice, rebuilt graph ---
  {
    ccb_graph *g = diamond_graph(record_run);
    char first[256];
    char second[256];
    plan_string(g, first, sizeof first);
    plan_string(g, second, sizeof second);
    CHECK_STR_EQ(first, second);
    CHECK_STR_EQ(first, "a b c d ");
    ccb_graph_free(g);
    g = diamond_graph(record_run);
    plan_string(g, second, sizeof second);
    CHECK_STR_EQ(first, second);
    ccb_graph_free(g);
  }

  // --- insertion order is the documented tie-break ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    char plan[64];
    plan_string(g, plan, sizeof plan);
    CHECK_STR_EQ(plan, "b a ");
    ccb_graph_free(g);
  }

  // --- chain added in reverse insertion order still plans in order ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *c = ccb_graph_add_task(g, "c", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_dep(c, "b");
    ccb_task_add_dep(b, "a");
    const char *want[] = { "a", "b", "c" };
    check_plan_order(g, want, 3);
    check_plan_valid(g);
    ccb_graph_free(g);
  }

  // --- layered synthetic graph: valid, deterministic, affected ---
  {
    ccb_graph *g = layered_graph();
    CHECK(ccb_graph_validate(g, &diags) == CCB_OK);
    CHECK(g->count == 51);
    char first[1024];
    char second[1024];
    plan_string(g, first, sizeof first);
    plan_string(g, second, sizeof second);
    CHECK_STR_EQ(first, second);
    check_plan_valid(g);
    ccb_graph_free(g);
    g = layered_graph();
    plan_string(g, second, sizeof second);
    CHECK_STR_EQ(first, second);
    char **ids = NULL;
    size_t nids = 0;
    const char *changed[] = { "out-0-0" };
    CHECK(ccb_graph_affected(g, changed, 1, &ids, &nids) ==
      CCB_OK);
    CHECK(nids == 5);
    if (nids == 5) {
      CHECK_STR_EQ(ids[0], "c1_0");
      CHECK_STR_EQ(ids[3], "c4_0");
      CHECK_STR_EQ(ids[4], "join");
    }
    ccb_free_str_array(ids, nids);
    ccb_graph_free(g);
  }

  // --- every problem reported at once; cycle skipped when a dep
  // is unknown (the cycle check needs resolvable dependencies) ---
  {
    ccb_graph *g = ccb_graph_create();
    ccb_task *a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_dep(a, "ghost1");
    ccb_task_add_dep(a, "ghost2");
    size_t before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK(diags.count == before + 2);
    CHECK(diag_has_code(&diags, "CCB-E1013"));
    ccb_graph_free(g);
    g = ccb_graph_create();
    a = ccb_graph_add_task(g, "a", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task *b = ccb_graph_add_task(g, "b", "test",
      CCB_TASK_CACHE_FINGERPRINT, record_run, NULL);
    ccb_task_add_dep(a, "b");
    ccb_task_add_dep(b, "a");
    ccb_task_add_dep(a, "ghost");
    before = diags.count;
    CHECK(ccb_graph_validate(g, &diags) == CCB_ERR_CONFIG);
    CHECK(diag_has_code(&diags, "CCB-E1013"));
    int e1014 = 0;
    for (size_t i = before; i < diags.count; i++) {
      if (strcmp(diags.items[i].code, "CCB-E1014") == 0) {
        e1014 = 1;
      }
    }
    CHECK(e1014 == 0);
    ccb_graph_free(g);
  }

  // --- pipeline builder deduplicates repeated config paths ---
  // The same AAR listed twice, and a java_extra dir equal to the
  // java dir, must not trip the engine's duplicate-declaration
  // check: the builder declares each source input once.
  {
    ccb_config *cfg = fake_config(0);
    cfg->aars = ccb_xcalloc(2, sizeof(char *));
    cfg->aars[0] = ccb_xstrdup("x.aar");
    cfg->aars[1] = ccb_xstrdup("x.aar");
    cfg->aars_count = 2;
    cfg->java_extra_dirs = ccb_xcalloc(1, sizeof(char *));
    cfg->java_extra_dirs[0] = ccb_xstrdup("src/main/java");
    cfg->java_extra_dirs_count = 1;
    ccb_diag_list gdiags;
    ccb_diag_list_init(&gdiags);
    ccb_graph *g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    CHECK(gdiags.count == 0);
    if (g != NULL) {
      ccb_task *deps = ccb_graph_find(g, "deps");
      CHECK(deps != NULL);
      if (deps != NULL) {
        // project-model + toolchain + one deduped AAR.
        CHECK(deps->inputs_count == 3);
      }
      ccb_task *code = ccb_graph_find(g, "compile-code");
      CHECK(code != NULL);
      if (code != NULL) {
        size_t java_seen = 0;
        for (size_t i = 0; i < code->inputs_count; i++) {
          if (strcmp(code->inputs[i].name, "src/main/java") == 0) {
            java_seen++;
          }
        }
        CHECK(java_seen == 1);
      }
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
  }

  // --- pipeline builder declares the files each step reads ---
  {
    ccb_config *cfg = fake_config(0);
    cfg->jars = ccb_xcalloc(1, sizeof(char *));
    cfg->jars[0] = ccb_xstrdup("lib.jar");
    cfg->jars_count = 1;
    cfg->kotlin_plugin_jars = ccb_xcalloc(1, sizeof(char *));
    cfg->kotlin_plugin_jars[0] = ccb_xstrdup("plugin.jar");
    cfg->kotlin_plugin_jars_count = 1;
    cfg->minify = 1;
    cfg->proguard_rules = ccb_xstrdup("rules.pro");
    ccb_diag_list gdiags;
    ccb_diag_list_init(&gdiags);
    ccb_graph *g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      ccb_task *code = ccb_graph_find(g, "compile-code");
      CHECK(code != NULL);
      if (code != NULL) {
        CHECK(task_has_input(code, "lib.jar",
          CCB_TASK_INPUT_SOURCE));
        CHECK(task_has_input(code, "plugin.jar",
          CCB_TASK_INPUT_SOURCE));
      }
      ccb_task *dex = ccb_graph_find(g, "dex");
      CHECK(dex != NULL);
      if (dex != NULL) {
        CHECK(task_has_input(dex, "lib.jar",
          CCB_TASK_INPUT_SOURCE));
        CHECK(task_has_input(dex, "rules.pro",
          CCB_TASK_INPUT_SOURCE));
      }
      ccb_graph_free(g);
    }
    // Without minification the ProGuard rules are not read, so
    // they are not declared either.
    cfg->minify = 0;
    g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      ccb_task *dex = ccb_graph_find(g, "dex");
      CHECK(dex != NULL);
      if (dex != NULL) {
        CHECK(!task_has_input(dex, "rules.pro",
          CCB_TASK_INPUT_SOURCE));
      }
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
  }

  // --- native declares NDK sources and CMakeLists.txt only when
  // the step reads them ---
  {
    ccb_config *cfg = fake_config(0);
    cfg->ndk_sources = ccb_xcalloc(1, sizeof(char *));
    cfg->ndk_sources[0] = ccb_xstrdup("src/main/cpp");
    cfg->ndk_sources_count = 1;
    cfg->cmake_lists = ccb_xstrdup("CMakeLists.txt");
    cfg->has_ndk = 0;
    cfg->has_cmake = 0;
    ccb_diag_list gdiags;
    ccb_diag_list_init(&gdiags);
    ccb_graph *g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      ccb_task *native = ccb_graph_find(g, "native");
      CHECK(native != NULL);
      if (native != NULL) {
        CHECK(!task_has_input(native, "src/main/cpp",
          CCB_TASK_INPUT_SOURCE));
        CHECK(!task_has_input(native, "CMakeLists.txt",
          CCB_TASK_INPUT_SOURCE));
      }
      ccb_graph_free(g);
    }
    cfg->has_ndk = 1;
    cfg->has_cmake = 1;
    g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      ccb_task *native = ccb_graph_find(g, "native");
      CHECK(native != NULL);
      if (native != NULL) {
        CHECK(task_has_input(native, "src/main/cpp",
          CCB_TASK_INPUT_SOURCE));
        CHECK(task_has_input(native, "CMakeLists.txt",
          CCB_TASK_INPUT_SOURCE));
      }
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
  }

  // --- sign declares the keystore and the password variables ---
  {
    ccb_config *cfg = fake_config(0);
    cfg->variant = ccb_xstrdup("release");
    cfg->has_release_signing = 1;
    cfg->release_store = ccb_xstrdup("release.keystore");
    cfg->release_store_pw_env = ccb_xstrdup("MY_STORE_PW");
    cfg->release_key_pw_env = ccb_xstrdup("MY_KEY_PW");
    ccb_diag_list gdiags;
    ccb_diag_list_init(&gdiags);
    ccb_graph *g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      ccb_task *sign = ccb_graph_find(g, "sign");
      CHECK(sign != NULL);
      if (sign != NULL) {
        CHECK(task_has_input(sign, "release.keystore",
          CCB_TASK_INPUT_SOURCE));
        CHECK(task_has_input(sign, "MY_STORE_PW",
          CCB_TASK_INPUT_ENV));
        CHECK(task_has_input(sign, "MY_KEY_PW",
          CCB_TASK_INPUT_ENV));
      }
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
    // A configured debug keystore is declared; the default one the
    // step generates when absent is not (documented gap).
    cfg = fake_config(0);
    cfg->debug_store = ccb_xstrdup("debug.keystore");
    ccb_diag_list_init(&gdiags);
    g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      ccb_task *sign = ccb_graph_find(g, "sign");
      CHECK(sign != NULL);
      if (sign != NULL) {
        CHECK(task_has_input(sign, "debug.keystore",
          CCB_TASK_INPUT_SOURCE));
        CHECK(!task_has_input(sign, "MY_STORE_PW",
          CCB_TASK_INPUT_ENV));
      }
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
    cfg = fake_config(0);
    ccb_diag_list_init(&gdiags);
    g = ccb_pipeline_graph_build(cfg, 0, &gdiags);
    CHECK(g != NULL);
    if (g != NULL) {
      ccb_task *sign = ccb_graph_find(g, "sign");
      CHECK(sign != NULL);
      if (sign != NULL) {
        CHECK(!task_has_input(sign, "debug.keystore",
          CCB_TASK_INPUT_SOURCE));
        CHECK(!task_has_input(sign, "release.keystore",
          CCB_TASK_INPUT_SOURCE));
      }
      ccb_graph_free(g);
    }
    ccb_diag_list_free(&gdiags);
    ccb_config_free(cfg);
  }

  // --- plan JSON carries input kind/required; affected emit ---
  {
    ccb_graph *g = diamond_graph(record_run);
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (f != NULL) {
      CHECK(ccb_graph_emit_plan_json(g, f) == CCB_OK);
      rewind(f);
      char buf[4096];
      size_t n = fread(buf, 1, sizeof buf - 1, f);
      buf[n] = '\0';
      CHECK(ccb_test_contains(buf,
        "\"name\":\"src-b\",\"kind\":\"source\",\"required\":true"));
      CHECK(ccb_test_contains(buf,
        "\"name\":\"out-b\",\"kind\":\"generated\",\"required\":true"));
      fclose(f);
      f = tmpfile();
      CHECK(ccb_graph_emit_affected_human(g, "src-b", f) ==
        CCB_OK);
      rewind(f);
      n = fread(buf, 1, sizeof buf - 1, f);
      buf[n] = '\0';
      CHECK(ccb_test_contains(buf, "tasks affected by 'src-b'"));
      CHECK(ccb_test_contains(buf, "  b\n"));
      CHECK(ccb_test_contains(buf, "  d\n"));
      fclose(f);
      f = tmpfile();
      CHECK(ccb_graph_emit_affected_json(g, "src-b", f) ==
        CCB_OK);
      rewind(f);
      n = fread(buf, 1, sizeof buf - 1, f);
      buf[n] = '\0';
      CHECK(ccb_test_contains(buf,
        "\"event\":\"affected\",\"input\":\"src-b\",\"tasks\":"
        "[\"b\",\"d\"]"));
      fclose(f);
      f = tmpfile();
      CHECK(ccb_graph_emit_affected_human(g, "nope", f) ==
        CCB_OK);
      rewind(f);
      n = fread(buf, 1, sizeof buf - 1, f);
      buf[n] = '\0';
      CHECK(ccb_test_contains(buf, "no tasks affected by 'nope'"));
      fclose(f);
    }
    ccb_graph_free(g);
  }

  ccb_diag_list_free(&diags);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
