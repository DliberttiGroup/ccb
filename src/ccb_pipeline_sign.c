// ccb_pipeline_sign: zipalign + sign + verify stages.
// Split from ccb_pipeline.c.

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_proc.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_cache.h"
#include "ccb_manifest.h"
#include "ccb_maven.h"
#include "ccb_net.h"
#include "ccb_resns.h"
#include "ccb_zip.h"
#include "ccb_bundle.h"
#include "ccb_dexcheck.h"
#include "ccb_daemon.h"
#include "ccb_pipeline.h"
#include <sys/stat.h>
#include <unistd.h>
#include "ccb_pipeline_internal.h"

// ---------- zipalign ----------

ccb_status ccb_step_align(ccb_pipe *p) {
  char *hash = ccb_cache_hash_file(p->unsigned_apk);
  if (hash == NULL && !p->opts->dry_run && !p->opts->explain) {
    ccb_pipe_diag(p, "CCB-E3008", "zipalign", "unsigned APK is missing (packaging must run first)");
    return CCB_ERR_BUILD;
  }
  const char *old = ccb_cache_step_fp(p->cache, "align");
  // The align fingerprint pairs the unsigned-APK hash with the
  // effective zipalign path, so switching the zipalign tool
  // invalidates the step even when the APK bytes are identical.
  ccb_str afp;
  ccb_str_init(&afp);
  ccb_str_append(&afp, p->sdk->zipalign == NULL ? "" : p->sdk->zipalign);
  ccb_str_append(&afp, "|");
  ccb_str_append(&afp, hash == NULL ? "" : hash);
  char *align_fp = ccb_str_steal(&afp);
  if (p->opts->explain) {
    if (p->package_will_run) {
      printf("align: run (packaging outputs will change)\n");
      p->align_will_run = 1;
    } else if (old != NULL && hash != NULL && strcmp(old, align_fp) == 0) {
      printf("align: skip (inputs unchanged)\n");
      p->align_will_run = 0;
    } else {
      printf("align: run (packaging outputs changed)\n");
      p->align_will_run = 1;
    }
    free(hash);
    free(align_fp);
    return CCB_OK;
  }
  if (!p->opts->dry_run && old != NULL && hash != NULL && strcmp(old, align_fp) == 0 &&
      ccb_path_exists(p->aligned_apk)) {
    free(hash);
    free(align_fp);
    p->align_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "align", 1, "packaging outputs unchanged");
    return CCB_OK;
  }
  p->align_will_run = 1;
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->zipalign);
  ccb_argv_push(&av, "-f");
  ccb_argv_push(&av, "4");
  ccb_argv_push(&av, p->unsigned_apk);
  ccb_argv_push(&av, p->aligned_apk);
  ccb_status rc = ccb_run_tool(p, "zipalign", "CCB-E3008", av.v, NULL,
    "check that the unsigned APK is a valid ZIP");
  free(av.v);
  if (rc != CCB_OK) {
    free(hash);
    free(align_fp);
    return CCB_ERR_BUILD;
  }
  if (!p->opts->dry_run && hash != NULL) {
    ccb_cache_set_step(p->cache, "align", align_fp);
    p->cache_dirty = 1;
  }
  free(align_fp);
  free(hash);
  p->steps_run++;
  return CCB_OK;
}

// ---------- sign ----------

// True when path is a whole signed artifact: a non-empty ZIP, for
// both APK and AAB (a bundle is a ZIP too). The signer truncates its
// output file on failure, so a later skip must not trust a 0-byte
// (or otherwise non-ZIP) file that merely exists: the build would
// skip signing forever and fail at verify instead of re-signing.
static int ccb_signed_artifact_valid(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4) {
    return 0;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return 0;
  }
  unsigned char head[4];
  size_t n = fread(head, 1, sizeof(head), f);
  fclose(f);
  return n == sizeof(head) && head[0] == 'P' && head[1] == 'K' &&
    head[2] == 0x03 && head[3] == 0x04;
}

static char *ccb_keytool_path(ccb_pipe *p) {
  char *home = ccb_java_home(p);
  char *kt = ccb_path_join(home, "bin/keytool");
  free(home);
  if (access(kt, X_OK) == 0) {
    return kt;
  }
  free(kt);
  return ccb_sdk_find_on_path("keytool");
}

ccb_status ccb_step_sign(ccb_pipe *p) {
  // AAB signing reuses the APK sign step verbatim (same keystore,
  // alias, and password rules, including debug keystore generation)
  // with two bundle differences: the input is the unaligned bundle
  // (bundles are not zipaligned) and apksigner gets --min-sdk-version
  // because it cannot read the manifest from base/manifest/.
  int is_aab = p->opts->aab;
  const char *sign_key = is_aab ? "sign-aab" : "sign";
  const char *sign_in = is_aab ? p->unsigned_aab : p->aligned_apk;
  char *apk_path = ccb_path_join(p->out_dir, is_aab ? "app.aab" : "app.apk");
  int is_debug = strcmp(p->cfg->variant, "debug") == 0;
  char *keystore = NULL;
  if (is_debug) {
    if (p->cfg->debug_store != NULL) {
      keystore = ccb_xstrdup(p->cfg->debug_store);
    } else {
      const char *home = getenv("HOME");
      keystore = home == NULL ? ccb_xstrdup(".android/debug.keystore")
        : ccb_path_join3(home, ".android", "debug.keystore");
    }
  } else {
    keystore = ccb_xstrdup(p->cfg->release_store);
  }
  const char *alias = is_debug ? p->cfg->debug_alias : p->cfg->release_alias;
  char *hash = ccb_cache_hash_file(sign_in);
  if (hash == NULL && !p->opts->dry_run && !p->opts->explain) {
    ccb_pipe_diag(p, "CCB-E3009", "sign", is_aab
      ? "unsigned AAB is missing (packaging must run first)"
      : "aligned APK is missing (alignment must run first)");
    free(apk_path);
    free(keystore);
    return CCB_ERR_SIGNING;
  }
  ccb_str fpbuf;
  ccb_str_init(&fpbuf);
  ccb_str_append(&fpbuf, p->cfg->variant);
  ccb_str_append_c(&fpbuf, '|');
  ccb_str_append(&fpbuf, keystore);
  ccb_str_append_c(&fpbuf, '|');
  ccb_str_append(&fpbuf, hash == NULL ? "" : hash);
  char *keystore_hash = ccb_cache_hash_file(keystore);
  if (keystore_hash != NULL) {
    ccb_str_append_c(&fpbuf, '|');
    ccb_str_append(&fpbuf, keystore_hash);
    free(keystore_hash);
  }
  // The alias selects the signing key: changing it must re-sign, or
  // the build ships an APK signed with the previous key (the keystore
  // path and hash alone cannot see the switch).
  ccb_str_append_c(&fpbuf, '|');
  ccb_str_append(&fpbuf, alias == NULL ? "" : alias);
  char *fp = ccb_str_steal(&fpbuf);
  const char *old = ccb_cache_step_fp(p->cache, sign_key);
  if (p->opts->explain) {
    if (p->align_will_run || (is_aab && p->package_will_run)) {
      printf("sign: run (%s will change)\n", is_aab ? "unsigned AAB" : "aligned APK");
      p->sign_will_run = 1;
    } else if (old != NULL && strcmp(old, fp) == 0) {
      printf("sign: skip (inputs unchanged)\n");
      p->sign_will_run = 0;
    } else {
      printf("sign: run (signing inputs changed)\n");
      p->sign_will_run = 1;
    }
    free(fp);
    free(hash);
    free(apk_path);
    free(keystore);
    return CCB_OK;
  }
  if (!p->opts->dry_run && old != NULL && strcmp(old, fp) == 0 &&
      ccb_signed_artifact_valid(apk_path)) {
    free(fp);
    free(hash);
    free(apk_path);
    free(keystore);
    p->sign_will_run = 0;
    p->steps_skipped++;
    ccb_log_cache(&p->log, "sign", 1, "signing inputs unchanged");
    return CCB_OK;
  }
  p->sign_will_run = 1;
  // Debug keystore: create the standard one when it does not exist yet.
  if (is_debug && !ccb_path_exists(keystore) && !p->opts->dry_run) {
    char *parent = ccb_dirname(keystore);
    if (ccb_mkdir_p(parent) != CCB_OK) {
      free(parent);
      ccb_pipe_diag(p, "CCB-E3009", "sign", "cannot create debug keystore directory");
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
    free(parent);
    char *keytool = ccb_keytool_path(p);
    if (keytool == NULL) {
      ccb_pipe_diag(p, "CCB-E3009", "sign", "keytool not found, cannot create debug keystore");
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
    CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, keytool);
    ccb_argv_push(&av, "-genkeypair");
    ccb_argv_push(&av, "-keystore");
    ccb_argv_push(&av, keystore);
    ccb_argv_push(&av, "-alias");
    ccb_argv_push(&av, alias);
    ccb_argv_push(&av, "-keyalg");
    ccb_argv_push(&av, "RSA");
    ccb_argv_push(&av, "-keysize");
    ccb_argv_push(&av, "2048");
    ccb_argv_push(&av, "-validity");
    ccb_argv_push(&av, "10000");
    ccb_argv_push(&av, "-storepass");
    ccb_argv_push(&av, "android");
    ccb_argv_push(&av, "-keypass");
    ccb_argv_push(&av, "android");
    ccb_argv_push(&av, "-dname");
    ccb_argv_push(&av, "CN=Android Debug,O=Android,C=US");
    char **kenv = ccb_jvm_env(p, ar);
    ccb_status rc = ccb_run_tool(p, "sign", "CCB-E3009", av.v, kenv,
      "check that keytool can write the debug keystore");
    free(av.v);
    free(keytool);
    if (rc != CCB_OK) {
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
  }
  {
    CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, p->sdk->apksigner);
    ccb_argv_push(&av, "sign");
    if (is_aab) {
      ccb_argv_push(&av, "--min-sdk-version");
      ccb_argv_pushf(&av, "%ld", p->cfg->min_sdk);
    }
    ccb_argv_push(&av, "--ks");
    ccb_argv_push(&av, keystore);
    ccb_argv_push(&av, "--ks-key-alias");
    ccb_argv_push(&av, alias);
    ccb_argv_push(&av, "--ks-pass");
    if (is_debug) {
      ccb_argv_push(&av, "pass:android");
    } else {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "env:");
      ccb_str_append(&s, p->cfg->release_store_pw_env);
      ccb_argv_push(&av, s.buf);
      ccb_str_free(&s);
    }
    ccb_argv_push(&av, "--key-pass");
    if (is_debug) {
      ccb_argv_push(&av, "pass:android");
    } else {
      ccb_str s;
      ccb_str_init(&s);
      ccb_str_append(&s, "env:");
      ccb_str_append(&s, p->cfg->release_key_pw_env);
      ccb_argv_push(&av, s.buf);
      ccb_str_free(&s);
    }
    ccb_argv_push(&av, "--out");
    ccb_argv_push(&av, apk_path);
    ccb_argv_push(&av, sign_in);
    char **senv = ccb_jvm_env(p, ar);
    ccb_status rc = ccb_run_tool(p, "sign", "CCB-E3009", av.v, senv,
      "check the keystore, alias, and passwords");
    free(av.v);
    if (rc != CCB_OK) {
      free(fp);
      free(hash);
      free(apk_path);
      free(keystore);
      return CCB_ERR_SIGNING;
    }
  }
  if (!p->opts->dry_run) {
    ccb_cache_set_step(p->cache, sign_key, fp);
    p->cache_dirty = 1;
  }
  free(fp);
  free(hash);
  free(apk_path);
  free(keystore);
  p->steps_run++;
  return CCB_OK;
}

// ---------- verify ----------

// jarsigner lives next to keytool in the JDK; fall back to PATH like
// the keytool helper does.
static char *ccb_jarsigner_path(ccb_pipe *p) {
  char *home = ccb_java_home(p);
  char *js = ccb_path_join(home, "bin/jarsigner");
  free(home);
  if (access(js, X_OK) == 0) {
    return js;
  }
  free(js);
  return ccb_sdk_find_on_path("jarsigner");
}

ccb_status ccb_step_verify(ccb_pipe *p) {
  // apksigner verify hard-requires AndroidManifest.xml at the ZIP
  // root, so it cannot verify a bundle (manifest lives at
  // base/manifest/). AAB verification is therefore jarsigner -verify
  // over the v1 JAR signature apksigner wrote, plus a structural
  // check that the required bundle entries are present.
  int is_aab = p->opts->aab;
  const char *verify_key = is_aab ? "verify-aab" : "verify";
  const char *fmt_label = is_aab ? "AAB" : "APK";
  char *apk_path = ccb_path_join(p->out_dir, is_aab ? "app.aab" : "app.apk");
  char *hash = ccb_cache_hash_file(apk_path);
  if (hash == NULL && !p->opts->dry_run && !p->opts->explain) {
    ccb_str msg;
    ccb_str_init(&msg);
    ccb_str_append(&msg, "signed ");
    ccb_str_append(&msg, fmt_label);
    ccb_str_append(&msg, " is missing (signing must run first)");
    ccb_pipe_diag(p, "CCB-E3010", "verify", msg.buf);
    ccb_str_free(&msg);
    free(apk_path);
    return CCB_ERR_BUILD;
  }
  const char *old = ccb_cache_step_fp(p->cache, verify_key);
  if (p->opts->explain) {
    if (p->sign_will_run) {
      printf("verify: run (signed %s will change)\n", fmt_label);
    } else if (old != NULL && hash != NULL && strcmp(old, hash) == 0) {
      printf("verify: skip (%s unchanged)\n", fmt_label);
    } else {
      printf("verify: run (%s changed)\n", fmt_label);
    }
    free(hash);
    free(apk_path);
    return CCB_OK;
  }
  if (!p->opts->dry_run && old != NULL && hash != NULL && strcmp(old, hash) == 0) {
    free(hash);
    free(apk_path);
    p->steps_skipped++;
    ccb_log_cache(&p->log, "verify", 1, "signed output unchanged");
    return CCB_OK;
  }
  if (is_aab) {
    char *jarsigner = ccb_jarsigner_path(p);
    if (jarsigner == NULL) {
      ccb_pipe_diag(p, "CCB-E3010", "verify", "jarsigner not found, cannot verify AAB");
      free(hash);
      free(apk_path);
      return CCB_ERR_BUILD;
    }
    CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_argv_push(&av, jarsigner);
    ccb_argv_push(&av, "-verify");
    ccb_argv_push(&av, apk_path);
    char **venv = ccb_jvm_env(p, ar);
    ccb_status rc = ccb_run_tool(p, "verify", "CCB-E3010", av.v, venv,
      "the AAB failed JAR signature verification; rebuild from clean");
    free(av.v);
    free(jarsigner);
    if (rc != CCB_OK) {
      free(hash);
      free(apk_path);
      return CCB_ERR_BUILD;
    }
    // Structural check: the entries bundletool and Play require.
    ccb_zip_archive *aab = NULL;
    int entries_ok = 0;
    if (ccb_zip_open(apk_path, &aab) == CCB_OK) {
      int has_manifest = ccb_zip_find(aab, "base/manifest/AndroidManifest.xml") != NULL;
      int has_config = ccb_zip_find(aab, "BundleConfig.pb") != NULL;
      int has_sig = 0;
      for (size_t i = 0; i < aab->count; i++) {
        if (strncmp(aab->entries[i].name, "META-INF/", 9) == 0) {
          has_sig = 1;
          break;
        }
      }
      entries_ok = has_manifest && has_config && has_sig;
      ccb_zip_close(aab);
    }
    if (!entries_ok) {
      ccb_pipe_diag(p, "CCB-E3010", "verify",
        "AAB is missing required entries (base/manifest/AndroidManifest.xml, "
        "BundleConfig.pb, or META-INF signatures)");
      free(hash);
      free(apk_path);
      return CCB_ERR_BUILD;
    }
    if (!p->opts->dry_run && hash != NULL) {
      ccb_cache_set_step(p->cache, verify_key, hash);
      p->cache_dirty = 1;
    }
    free(hash);
    free(apk_path);
    p->steps_run++;
    return CCB_OK;
  }
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();
  ccb_argv av;
  memset(&av, 0, sizeof(av));
  av.ar = ar;
  ccb_argv_push(&av, p->sdk->apksigner);
  ccb_argv_push(&av, "verify");
  ccb_argv_push(&av, apk_path);
  char **venv = ccb_jvm_env(p, ar);
  ccb_status rc = ccb_run_tool(p, "verify", "CCB-E3010", av.v, venv,
    "the APK failed signature verification; rebuild from clean");
  free(av.v);
  if (rc != CCB_OK) {
    free(hash);
    free(apk_path);
    return CCB_ERR_BUILD;
  }
  CCB_AUTO_ARENA ccb_arena *ar2 = ccb_arena_create();
  ccb_argv av2;
  memset(&av2, 0, sizeof(av2));
  av2.ar = ar2;
  ccb_argv_push(&av2, p->sdk->aapt2);
  ccb_argv_push(&av2, "dump");
  ccb_argv_push(&av2, "badging");
  ccb_argv_push(&av2, apk_path);
  rc = ccb_run_tool(p, "verify", "CCB-E3011", av2.v, NULL,
    "the APK failed aapt2 badging; it is not a well-formed app package");
  free(av2.v);
  if (rc != CCB_OK) {
    free(hash);
    free(apk_path);
    return CCB_ERR_BUILD;
  }
  if (!p->opts->dry_run && hash != NULL) {
    ccb_cache_set_step(p->cache, verify_key, hash);
    p->cache_dirty = 1;
  }
  free(hash);
  free(apk_path);
  p->steps_run++;
  return CCB_OK;
}
