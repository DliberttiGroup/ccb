// Tests for ccb_proc (spawn + capture + render).
#include "ccb_common.h"
#include "test.h"
#include "ccb_proc.h"
#include "ccb_str.h"

int main(void) {
  // Successful command with captured output.
  {
    char *argv[] = { "/bin/echo", "hello", "world", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK);
    CHECK(r.exit_code == 0);
    CHECK_STR_EQ(r.out, "hello world\n");
    ccb_proc_result_free(&r);
  }
  // Nonzero exit is reported, not treated as a spawn failure.
  {
    char *argv[] = { "/bin/sh", "-c", "echo out; echo err >&2; exit 3", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK);
    CHECK(r.exit_code == 3);
    CHECK_STR_EQ(r.out, "out\n");
    CHECK_STR_EQ(r.err, "err\n");
    ccb_proc_result_free(&r);
  }
  // Missing binary is a spawn failure (exit 127 from the child shim).
  {
    char *argv[] = { "/nonexistent/ccb-tool", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK);
    CHECK(r.exit_code == 127);
    ccb_proc_result_free(&r);
  }
  // Extra environment overrides inherited variables.
  {
    char *argv[] = { "/bin/sh", "-c", "echo $CCB_TEST_VAR", NULL };
    char *env[] = { "CCB_TEST_VAR=from-extra", NULL };
    ccb_proc_result r;
    CHECK(ccb_proc_run(argv, NULL, env, &r) == CCB_OK);
    CHECK_STR_EQ(r.out, "from-extra\n");
    ccb_proc_result_free(&r);
  }
  // Bare "K" entries remove the variable from the child's environment.
  {
    char *argv[] = { "/bin/sh", "-c", "echo \"x${CCB_TEST_REMOVE}y\"", NULL };
    ccb_proc_result r;
    setenv("CCB_TEST_REMOVE", "present", 1);
    CHECK(ccb_proc_run(argv, NULL, NULL, &r) == CCB_OK);
    CHECK_STR_EQ(r.out, "xpresenty\n");
    ccb_proc_result_free(&r);
    char *env2[] = { "CCB_TEST_REMOVE", NULL };
    CHECK(ccb_proc_run(argv, NULL, env2, &r) == CCB_OK);
    CHECK_STR_EQ(r.out, "xy\n");
    ccb_proc_result_free(&r);
    unsetenv("CCB_TEST_REMOVE");
  }
  // Render quoting.
  {
    char *argv[] = { "kotlinc", "-cp", "a b/c.jar", "it's", NULL };
    ccb_str out;
    ccb_str_init(&out);
    ccb_proc_render(argv, &out);
    CHECK_STR_EQ(out.buf, "kotlinc -cp 'a b/c.jar' 'it'\\''s'");
    ccb_str_free(&out);
  }
  TEST_MAIN_END();
}
