// ccb_proc: spawn tools and capture output. No system() calls.
// The child inherits the environment plus env_extra entries ("K=V"),
// which override same-named inherited variables. The child's stdin is
// /dev/null so tools can never block waiting for terminal input.
#ifndef CCB_PROC_H
#define CCB_PROC_H

#include "ccb_common.h"
#include "ccb_str.h"

typedef struct {
  int exit_code;
  int signaled;
  char *out;
  char *err;
} ccb_proc_result;

void ccb_proc_result_free(ccb_proc_result *r);
// Run argv (NULL-terminated) in cwd (NULL = inherit). Returns CCB_OK if
// the child ran, even when it exited nonzero; check result.exit_code.
// Returns CCB_ERR_IO when spawning itself failed.
// env_extra entries are "K=V" (added or overriding) or bare "K" (the
// variable is REMOVED from the child's environment).
ccb_status ccb_proc_run(char *const argv[], const char *cwd, char *const env_extra[], ccb_proc_result *result);
// Run argv with stdio inherited (for streaming commands like logcat).
// Returns the child's exit code, or -1 when spawning failed.
int ccb_proc_run_inherit(char *const argv[]);
// Render argv as a shell-quoted command line (dry-run + diagnostics).
void ccb_proc_render(char *const argv[], ccb_str *out);

#endif
