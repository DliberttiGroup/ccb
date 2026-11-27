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

  ccb_diag_list_free(&diags);
  ccb_remove_recursive(BASE);
  TEST_MAIN_END();
}
