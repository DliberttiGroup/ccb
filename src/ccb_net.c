// ccb_net: minimal network downloads (see ccb_net.h).
#include "ccb_common.h"

#include "ccb_fs.h"
#include "ccb_net.h"
#include "ccb_proc.h"
#include "ccb_sdk.h"
#include "ccb_str.h"

int ccb_net_download_auth(const char *url, const char *dest,
    const char *user, const char *pass) {
  char *curl = ccb_sdk_find_on_path("curl");
  if (curl == NULL) {
    return -1;
  }
  // The credential is one argv element, built here and freed after
  // the run; it never enters a URL, a log line, or a diagnostic.
  char *cred = NULL;
  char *argv[9];
  int i = 0;
  argv[i++] = curl;
  argv[i++] = "-sL";
  argv[i++] = "--fail";
  argv[i++] = "-o";
  argv[i++] = (char *)dest;
  if (user != NULL) {
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, user);
    ccb_str_append_c(&s, ':');
    ccb_str_append(&s, pass != NULL ? pass : "");
    cred = ccb_str_steal(&s);
    argv[i++] = "-u";
    argv[i++] = cred;
  }
  argv[i++] = (char *)url;
  argv[i] = NULL;
  ccb_proc_result r;
  ccb_status rc = ccb_proc_run(argv, NULL, NULL, &r);
  int ok = rc == CCB_OK && r.exit_code == 0;
  ccb_proc_result_free(&r);
  free(cred);
  free(curl);
  return ok ? 0 : -1;
}

int ccb_net_download(const char *url, const char *dest) {
  return ccb_net_download_auth(url, dest, NULL, NULL);
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
