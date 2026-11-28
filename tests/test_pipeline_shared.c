// Tests for the shared compile providers (Step 6, audit D1): the
// app steps and the module builder must run the same argv
// construction. A fabricated pipe (no toolchain, no files) pins the
// exact command lines each provider builds for the app and module
// variants, including the lists that genuinely differ between them
// (upstream R.java files, R.txt, --proto-format).

#include "test.h"
#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_config.h"
#include "ccb_sdk.h"
#include "ccb_pipeline_internal.h"

// Join a NULL-terminated argv with single spaces. Owned.
static char *join_argv(char **argv) {
  ccb_str s;
  ccb_str_init(&s);
  for (size_t i = 0; argv[i] != NULL; i++) {
    if (i > 0) {
      ccb_str_append_c(&s, ' ');
    }
    ccb_str_append(&s, argv[i]);
  }
  return ccb_str_steal(&s);
}

int main(void) {
  ccb_config cfg;
  ccb_sdk sdk;
  ccb_pipe p;
  memset(&cfg, 0, sizeof(cfg));
  memset(&sdk, 0, sizeof(sdk));
  memset(&p, 0, sizeof(p));
  p.cfg = &cfg;
  p.sdk = &sdk;
  cfg.min_sdk = 26;
  cfg.target_sdk = 35;
  cfg.jvm_target = "17";
  sdk.aapt2 = "aapt2";
  sdk.android_jar = "android.jar";
  sdk.kotlinc = "kotlinc";
  sdk.javac = "javac";
  CCB_AUTO_ARENA ccb_arena *ar = ccb_arena_create();

  // Classpath: android.jar + resolved [deps] jars + AAR classes.jars,
  // in that order, for both paths.
  {
    char *jars[] = { "libs/a.jar", "libs/b.jar" };
    char *aars[] = { "mod/classes.jar" };
    p.resolved.jars = jars;
    p.resolved.jars_count = 2;
    p.aar_class_jars = aars;
    p.aar_class_jars_count = 1;
    char *cp = ccb_build_classpath(&p);
    CHECK_STR_EQ(cp, "android.jar:libs/a.jar:libs/b.jar:mod/classes.jar");
    free(cp);
    p.resolved.jars_count = 0;
    p.aar_class_jars_count = 0;
    cp = ccb_build_classpath(&p);
    CHECK_STR_EQ(cp, "android.jar");
    free(cp);
    p.resolved.jars = jars;
    p.resolved.jars_count = 2;
    p.aar_class_jars = aars;
    p.aar_class_jars_count = 1;
  }

  // aapt2 compile: identical for app and module.
  {
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_aapt2_compile_argv(&p, &av, "src/main/res", "out.zip");
    char *cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "aapt2 compile --dir src/main/res -o out.zip");
    free(cmd);
    free(av.v);
  }

  // aapt2 link: app APK, app AAB, module. The module line carries
  // --output-text-symbols between --java and --auto-add-overlay;
  // the AAB line carries --proto-format right after "link".
  {
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_aapt2_link_argv(&p, &av, "linked.ap_", "AndroidManifest.xml",
      "gen", NULL, 0);
    char *cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "aapt2 link -o linked.ap_ --manifest "
      "AndroidManifest.xml -I android.jar --java gen "
      "--auto-add-overlay --min-sdk-version 26 --target-sdk-version 35");
    free(cmd);
    free(av.v);
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_aapt2_link_argv(&p, &av, "linked-aab.ap_",
      "AndroidManifest.xml", "gen", NULL, 1);
    cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "aapt2 link --proto-format -o linked-aab.ap_ "
      "--manifest AndroidManifest.xml -I android.jar --java gen "
      "--auto-add-overlay --min-sdk-version 26 --target-sdk-version 35");
    free(cmd);
    free(av.v);
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_aapt2_link_argv(&p, &av, "lib.ap_", "lib/AndroidManifest.xml",
      "mod/gen", "mod/R.txt", 0);
    cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "aapt2 link -o lib.ap_ --manifest "
      "lib/AndroidManifest.xml -I android.jar --java mod/gen "
      "--output-text-symbols mod/R.txt --auto-add-overlay "
      "--min-sdk-version 26 --target-sdk-version 35");
    free(cmd);
    free(av.v);
  }

  // kotlinc: plugin args sit between -cp and the sources; the module
  // line adds its upstream dependency R.java files between the
  // generated R.java and the Java sources.
  {
    char *plug_jars[] = { "plug.jar" };
    char *plug_opts[] = { "id:key=value" };
    p.resolved.plugin_jars = plug_jars;
    p.resolved.plugin_jars_count = 1;
    cfg.kotlin_plugin_options = plug_opts;
    cfg.kotlin_plugin_options_count = 1;
    char *kt[] = { "Main.kt" };
    char *rjava[] = { "gen/R.java" };
    char *java[] = { "App.java" };
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_kotlinc_argv(&p, &av, "classes", "CP", kt, 1, rjava, 1,
      NULL, 0, java, 1);
    char *cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "kotlinc -jvm-target 17 -d classes -cp CP "
      "-Xplugin=plug.jar -P plugin:id:key=value Main.kt gen/R.java "
      "App.java");
    free(cmd);
    free(av.v);
    char *up[] = { "dep/R.java" };
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_kotlinc_argv(&p, &av, "mod/classes", "CP", kt, 1, rjava, 1,
      up, 1, java, 1);
    cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "kotlinc -jvm-target 17 -d mod/classes -cp CP "
      "-Xplugin=plug.jar -P plugin:id:key=value Main.kt gen/R.java "
      "dep/R.java App.java");
    free(cmd);
    free(av.v);
    p.resolved.plugin_jars_count = 0;
    cfg.kotlin_plugin_options_count = 0;
  }

  // javac: -cp is <out>:<cp>. The app compiles generated R.java plus
  // Java sources; a module compiles Java sources only (its R.class
  // is stripped from classes.jar).
  {
    char *rjava[] = { "gen/R.java" };
    char *java[] = { "App.java" };
    ccb_argv av;
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_javac_argv(&p, &av, "classes", "CP", rjava, 1, java, 1);
    char *cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "javac -encoding UTF-8 -source 17 -target 17 "
      "-cp classes:CP -d classes gen/R.java App.java");
    free(cmd);
    free(av.v);
    char *mjava[] = { "BaseJava.java" };
    memset(&av, 0, sizeof(av));
    av.ar = ar;
    ccb_javac_argv(&p, &av, "mod/classes", "CP", mjava, 1, NULL, 0);
    cmd = join_argv(av.v);
    CHECK_STR_EQ(cmd, "javac -encoding UTF-8 -source 17 -target 17 "
      "-cp mod/classes:CP -d mod/classes BaseJava.java");
    free(cmd);
    free(av.v);
  }

  TEST_MAIN_END();
}
