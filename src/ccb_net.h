// ccb_net: minimal network downloads, shared by setup and Maven
// resolution. The only transport is the `curl` binary spawned through
// ccb_proc_run (no HTTP library, no threads): curl follows redirects
// and --fail turns HTTP errors into a nonzero exit, so a 404 is a
// plain download failure, never a truncated file mistaken for success.
// Callers that need caching or checksums build on top of this.
#ifndef CCB_NET_H
#define CCB_NET_H

#include "ccb_common.h"

// Download url to dest (overwriting). Returns 0 on success, -1 when
// curl is missing or exits nonzero. dest is left untouched on
// failure only when curl itself fails before writing; callers that
// need atomicity download to a temp path and rename.
int ccb_net_download(const char *url, const char *dest);
// Same, with HTTP Basic auth. The credentials travel only in the
// curl argv (as one "-u user:pass" argument): never in the URL, and
// never printed — ccb_proc_run does not log argv, and no caller may
// render this argv. A NULL user downloads without auth.
int ccb_net_download_auth(const char *url, const char *dest,
  const char *user, const char *pass);
// Cache root: $XDG_CACHE_HOME/ccb, else ~/.cache/ccb. NULL when
// neither base is available. The caller frees the result.
char *ccb_net_cache_root(void);

#endif
