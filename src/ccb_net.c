// ccb_net: minimal network downloads (see ccb_net.h).
#include "ccb_common.h"

#include "ccb_fs.h"
#include "ccb_net.h"
#include "ccb_proc.h"
#include "ccb_sdk.h"

int ccb_net_download(const char *url, const char *dest) {
  char *curl = ccb_sdk_find_on_path("curl");
  if (curl == NULL) {
    return -1;
  }
  char *argv[] = { curl, "-sL", "--fail", "-o", (char *)dest, (char *)url, NULL };
  ccb_proc_result r;
  ccb_status rc = ccb_proc_run(argv, NULL, NULL, &r);
  int ok = rc == CCB_OK && r.exit_code == 0;
  ccb_proc_result_free(&r);
  free(curl);
  return ok ? 0 : -1;
}

char *ccb_net_cache_root(void) {
  const char *xdg = getenv("XDG_CACHE_HOME");
  if (xdg != NULL && xdg[0] != '\0') {
    return ccb_path_join(xdg, "ccb");
  }
  const char *home = getenv("HOME");
  if (home != NULL && home[0] != '\0') {
    return ccb_path_join3(home, ".cache", "ccb");
  }
  return NULL;
}
