#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_fs.h"
#include "ccb_hash.h"
#include "ccb_json.h"
#include "ccb_net.h"
#include "ccb_maven.h"
#include "ccb_diag.h"
#include "ccb_log.h"
#include "ccb_proc.h"
#include "ccb_config.h"
#include "ccb_daemon.h"
#include "ccb_sdk.h"
#include "ccb_pipeline.h"
#include "ccb_zip.h"
#include "ccb_preview.h"
#include "ccb_preview_lsp.h"
#include "ccb_preview_tree.h"
#include "ccb_preview_walk.h"
#include "ccb_preview_watch.h"
#include "ccb_preview_gate.h"
#include "ccb_preview_diff.h"
#include "ccb_cli.h"
#include <unistd.h>
#include <sys/stat.h>
#include "ccb_cli_internal.h"

// ccb_cli implementation, part 1: arg parsing, diag output, init, setup.

// SHA-256 of the official cmdline-tools Linux zip used by `ccb setup`
// to bootstrap an SDK. Verified against the complete download
// (commandlinetools-linux-11076708_latest.zip, 153607504 bytes).

const char *CCB_USAGE =
  "usage: ccb [--json] [--verbose] [--config PATH] [--variant debug|release]\n"
  "           [--dry-run] [--explain] <command> [args]\n"
  "\n"
  "commands:\n"
  "  init [dir] [--package PKG] [--name NAME]  scaffold a new project\n"
  "  setup [--yes] [--upgrade] [--print-env]  install missing SDK components\n"
  "  check [--preview-gate] [--no-preview-gate]\n"
  "                                            validate config + toolchain\n"
  "  build [--preview-gate] [--no-preview-gate] [--aab]\n"
  "        [--kotlinc-daemon] [--no-kotlinc-daemon]\n"
  "                                            build the APK (or .aab with --aab)\n"
  "  graph [--aab] [--affected INPUT]          print the build plan (task graph\n"
  "                                            in execution order) without building;\n"
  "                                            with --affected, list the tasks a\n"
  "                                            changed input invalidates instead\n"
  "  clean                                     remove the build directory\n"
  "                                            (and stop the kotlinc daemon)\n"
  "  daemon stop                               stop the kotlinc daemon\n"
  "  doctor                                    report toolchain status\n"
  "  install                                   adb install the built APK\n"
  "  run                                       install + launch the app\n"
  "  logcat                                    stream device logs\n"
  "  preview [--ascii] [--width N] [--device phone|tablet] [--strict]\n"
  "          [--screen NAME] [--output FILE] [--update-goldens] [--walk]\n"
  "          [--watch] [--diff DIR] [--storyboard A,B] [--tree-live]\n"
  "          [screen]                              render a UI preview\n"
  "  lsp [--scenario NAME] [--trace FILE]     preview logic-tree server\n"
  "                                            (JSON-RPC over stdio)\n"
  "  version                                   print the version\n";

void ccb_print_diags_human(ccb_diag_list *diags) {
  for (size_t i = 0; i < diags->count; i++) {
    ccb_diag *d = &diags->items[i];
    fprintf(stderr, "error %s [%s]", d->code, d->phase);
    if (d->task_id != NULL) {
      fprintf(stderr, " [task %s]", d->task_id);
    }
    fprintf(stderr, ": %s\n", d->message);
    if (d->file != NULL) {
      fprintf(stderr, "  at %s", d->file);
      if (d->line > 0) {
        fprintf(stderr, ":%ld", d->line);
      }
      fprintf(stderr, "\n");
    }
    if (d->command != NULL) {
      fprintf(stderr, "  command: %s\n", d->command);
    }
    if (d->suggestion != NULL) {
      fprintf(stderr, "  suggestion: %s\n", d->suggestion);
    }
  }
}

static void ccb_print_diags_json(ccb_diag_list *diags) {
  for (size_t i = 0; i < diags->count; i++) {
    ccb_diag *d = &diags->items[i];
    ccb_str s;
    ccb_str_init(&s);
    ccb_str_append(&s, "{\"event\":\"diagnostic\",\"code\":");
    ccb_json_escape(&s, d->code);
    ccb_str_append(&s, ",\"phase\":");
    ccb_json_escape(&s, d->phase);
    ccb_str_append(&s, ",\"message\":");
    ccb_json_escape(&s, d->message);
    if (d->file != NULL) {
      ccb_str_append(&s, ",\"file\":");
      ccb_json_escape(&s, d->file);
      ccb_str_appendf(&s, ",\"line\":%ld", d->line);
    }
    if (d->command != NULL) {
      ccb_str_append(&s, ",\"command\":");
      ccb_json_escape(&s, d->command);
    }
    if (d->suggestion != NULL) {
      ccb_str_append(&s, ",\"suggestion\":");
      ccb_json_escape(&s, d->suggestion);
    }
    if (d->task_id != NULL) {
      ccb_str_append(&s, ",\"task\":");
      ccb_json_escape(&s, d->task_id);
    }
    ccb_str_append(&s, "}\n");
    fputs(s.buf, stdout);
    ccb_str_free(&s);
  }
}

void ccb_print_diags(ccb_args *a, ccb_diag_list *diags) {
  if (a->json) {
    ccb_print_diags_json(diags);
  } else {
    ccb_print_diags_human(diags);
  }
}

// Load config for commands that need it. Prints diags, returns NULL on
// failure (caller maps to exit code 2).
ccb_config *ccb_load_config_or_die(ccb_args *a, ccb_diag_list *diags) {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    ccb_diag d = ccb_diag_make("CCB-E1009", "config", "cannot determine working directory");
    ccb_diag_list_push(diags, &d);
    return NULL;
  }
  char *path = NULL;
  if (ccb_config_discover(cwd, a->config_path, &path) != CCB_OK) {
    ccb_diag d = ccb_diag_make("CCB-E1009", "config",
      a->config_path != NULL ? "config file not found" : "no ccb.toml found in this directory or any parent");
    ccb_diag_list_push(diags, &d);
    return NULL;
  }
  ccb_config *cfg = NULL;
  if (ccb_config_load(path, a->variant, &cfg, diags) != CCB_OK) {
    free(path);
    return NULL;
  }
  free(path);
  return cfg;
}

// ---------- init ----------

static int ccb_write_scaffold_file(const char *dir, const char *rel, const char *content, int *created) {
  char *path = ccb_path_join(dir, rel);
  if (ccb_path_exists(path)) {
    free(path);
    return 0;
  }
  char *parent = ccb_dirname(path);
  if (ccb_mkdir_p(parent) != CCB_OK) {
    free(parent);
    free(path);
    return -1;
  }
  free(parent);
  if (ccb_write_file(path, content, strlen(content)) != CCB_OK) {
    free(path);
    return -1;
  }
  free(path);
  *created = 1;
  return 0;
}

static int ccb_cmd_init(ccb_args *a) {
  const char *dir = a->positional == NULL ? "." : a->positional;
  const char *package = a->init_package == NULL ? "com.example.hello" : a->init_package;
  const char *name = a->init_name == NULL ? "hello" : a->init_name;
  if (strcmp(dir, ".") != 0 && ccb_mkdir_p(dir) != CCB_OK) {
    fprintf(stderr, "error: cannot create directory %s\n", dir);
    return 2;
  }
  char *toml_path = ccb_path_join(dir, "ccb.toml");
  if (ccb_path_exists(toml_path)) {
    fprintf(stderr, "error: %s is already a CCB project (ccb.toml exists)\n", dir);
    free(toml_path);
    return 2;
  }
  free(toml_path);
  // Package path for the Kotlin source tree.
  ccb_str pkg_path;
  ccb_str_init(&pkg_path);
  for (const char *c = package; *c != '\0'; c++) {
    ccb_str_append_c(&pkg_path, *c == '.' ? '/' : *c);
  }
  ccb_str toml;
  ccb_str_init(&toml);
  ccb_str_append(&toml, "ccb_version = 1\n\n[project]\nname = \"");
  ccb_str_append(&toml, name);
  ccb_str_append(&toml, "\"\npackage = \"");
  ccb_str_append(&toml, package);
  ccb_str_append(&toml, "\"\nversion = \"1.0\"\nversion_code = 1\nmin_sdk = 26\n"
    "target_sdk = 35\ncompile_sdk = 35\n\n[paths]\nmanifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\nkotlin = \"src/main/kotlin\"\n");
  ccb_str manifest;
  ccb_str_init(&manifest);
  ccb_str_append(&manifest, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"");
  ccb_str_append(&manifest, package);
  ccb_str_append(&manifest, "\">\n"
    "  <application android:label=\"@string/app_name\" android:theme=\"@style/AppTheme\">\n"
    "    <activity android:name=\".MainActivity\" android:exported=\"true\">\n"
    "      <intent-filter>\n"
    "        <action android:name=\"android.intent.action.MAIN\" />\n"
    "        <category android:name=\"android.intent.category.LAUNCHER\" />\n"
    "      </intent-filter>\n"
    "    </activity>\n"
    "  </application>\n"
    "</manifest>\n");
  ccb_str activity;
  ccb_str_init(&activity);
  ccb_str_append(&activity, "package ");
  ccb_str_append(&activity, package);
  ccb_str_append(&activity, "\n\nimport android.app.Activity\nimport android.os.Bundle\n\n"
    "class MainActivity : Activity() {\n"
    "  override fun onCreate(savedInstanceState: Bundle?) {\n"
    "    super.onCreate(savedInstanceState)\n"
    "    setContentView(R.layout.activity_main)\n"
    "  }\n"
    "}\n");
  ccb_str kt_path;
  ccb_str_init(&kt_path);
  ccb_str_append(&kt_path, "src/main/kotlin/");
  ccb_str_append(&kt_path, pkg_path.buf);
  ccb_str_append(&kt_path, "/MainActivity.kt");
  static const char *layout =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<LinearLayout xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "  android:layout_width=\"match_parent\"\n"
    "  android:layout_height=\"match_parent\"\n"
    "  android:gravity=\"center\"\n"
    "  android:orientation=\"vertical\">\n"
    "  <TextView android:layout_width=\"wrap_content\"\n"
    "    android:layout_height=\"wrap_content\"\n"
    "    android:text=\"@string/hello\" />\n"
    "</LinearLayout>\n";
  ccb_str strings;
  ccb_str_init(&strings);
  ccb_str_append(&strings, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n"
    "  <string name=\"app_name\">");
  ccb_str_append(&strings, name);
  ccb_str_append(&strings, "</string>\n"
    "  <string name=\"hello\">Hello from CCB!</string>\n</resources>\n");
  static const char *themes =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n"
    "  <style name=\"AppTheme\" parent=\"@android:style/Theme.Material.Light.NoActionBar\" />\n"
    "</resources>\n";
  static const char *gitignore = "/build/\n";
  ccb_str readme;
  ccb_str_init(&readme);
  ccb_str_append(&readme, "# ");
  ccb_str_append(&readme, name);
  ccb_str_append(&readme, "\n\nBuilt with CCB. Build the debug APK with:\n\n```\nccb build\n```\n");
  const char *rels[] = {
    "ccb.toml",
    "src/main/AndroidManifest.xml",
    NULL, // MainActivity.kt path (dynamic)
    "src/main/res/layout/activity_main.xml",
    "src/main/res/values/strings.xml",
    "src/main/res/values/themes.xml",
    ".gitignore",
    "README.md"
  };
  const char *contents[] = {
    toml.buf, manifest.buf, activity.buf, layout, strings.buf, themes, gitignore, readme.buf
  };
  int created_count = 0;
  int skipped_count = 0;
  for (int i = 0; i < 8; i++) {
    const char *rel = rels[i] == NULL ? kt_path.buf : rels[i];
    int created = 0;
    if (ccb_write_scaffold_file(dir, rel, contents[i], &created) != 0) {
      fprintf(stderr, "error: cannot write %s/%s\n", dir, rel);
      ccb_str_free(&pkg_path);
      ccb_str_free(&toml);
      ccb_str_free(&manifest);
      ccb_str_free(&activity);
      ccb_str_free(&kt_path);
      ccb_str_free(&strings);
      ccb_str_free(&readme);
      return 2;
    }
    if (created) {
      created_count++;
      if (!a->json) {
        printf("created %s/%s\n", dir, rel);
      }
    } else {
      skipped_count++;
    }
  }
  if (a->json) {
    printf("{\"event\":\"init\",\"dir\":");
    ccb_str s;
    ccb_str_init(&s);
    ccb_json_escape(&s, dir);
    printf("%s,\"created\":%d,\"skipped\":%d}\n", s.buf, created_count, skipped_count);
    ccb_str_free(&s);
  } else if (skipped_count > 0) {
    printf("%d files already existed and were left untouched\n", skipped_count);
  }
  if (!a->json) {
    printf("run `ccb build` in %s to build the app\n", dir);
  }
  ccb_str_free(&pkg_path);
  ccb_str_free(&toml);
  ccb_str_free(&manifest);
  ccb_str_free(&activity);
  ccb_str_free(&kt_path);
  ccb_str_free(&strings);
  ccb_str_free(&readme);
  return 0;
}

// ---------- dispatch ----------

static int ccb_parse_args(int argc, char **argv, ccb_args *a) {
  memset(a, 0, sizeof(*a));
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const char *value = NULL;
    char flag[64];
    if (strncmp(arg, "--", 2) == 0) {
      const char *eq = strchr(arg, '=');
      size_t flen = eq == NULL ? strlen(arg) : (size_t)(eq - arg);
      if (flen >= sizeof(flag)) {
        return -1;
      }
      memcpy(flag, arg, flen);
      flag[flen] = '\0';
      value = eq == NULL ? NULL : eq + 1;
      if (strcmp(flag, "--json") == 0) {
        a->json = 1;
      } else if (strcmp(flag, "--verbose") == 0) {
        a->verbose = 1;
      } else if (strcmp(flag, "--dry-run") == 0) {
        a->dry_run = 1;
      } else if (strcmp(flag, "--explain") == 0) {
        a->explain = 1;
      } else if (strcmp(flag, "--yes") == 0) {
        a->yes = 1;
      } else if (strcmp(flag, "--upgrade") == 0) {
        a->upgrade = 1;
      } else if (strcmp(flag, "--print-env") == 0) {
        a->print_env = 1;
      } else if (strcmp(flag, "--aab") == 0) {
        a->aab = 1;
      } else if (strcmp(flag, "--affected") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->graph_affected = value;
      } else if (strcmp(flag, "--config") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->config_path = value;
      } else if (strcmp(flag, "--variant") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->variant = value;
      } else if (strcmp(flag, "--package") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->init_package = value;
      } else if (strcmp(flag, "--name") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->init_name = value;
      } else if (strcmp(flag, "--ascii") == 0) {
        a->preview_ascii = 1;
      } else if (strcmp(flag, "--walk") == 0) {
        a->preview_walk = 1;
      } else if (strcmp(flag, "--watch") == 0) {
        a->preview_watch = 1;
      } else if (strcmp(flag, "--tree-live") == 0) {
        a->preview_tree_live = 1;
      } else if (strcmp(flag, "--strict") == 0) {
        a->preview_strict = 1;
      } else if (strcmp(flag, "--width") == 0) {
        char *end = NULL;
        long v = 0;
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        v = strtol(value, &end, 10);
        if (end == value || *end != '\0') {
          return -1;
        }
        a->preview_width = v;
        a->preview_width_set = 1;
      } else if (strcmp(flag, "--device") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        if (strcmp(value, "phone") != 0 && strcmp(value, "tablet") != 0) {
          return -1;
        }
        a->preview_device = value;
      } else if (strcmp(flag, "--output") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_output = value;
      } else if (strcmp(flag, "--update-goldens") == 0) {
        a->preview_update_goldens = 1;
      } else if (strcmp(flag, "--screen") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_screen = value;
      } else if (strcmp(flag, "--diff") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_diff = value;
      } else if (strcmp(flag, "--storyboard") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_storyboard = value;
      } else if (strcmp(flag, "--scenario") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_scenario = value;
      } else if (strcmp(flag, "--trace") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_trace = value;
      } else if (strcmp(flag, "--record") == 0) {
        if (value == NULL) {
          if (i + 1 >= argc) {
            return -1;
          }
          value = argv[++i];
        }
        a->preview_record = value;
      } else if (strcmp(flag, "--preview-gate") == 0) {
        a->preview_gate_on = 1;
      } else if (strcmp(flag, "--no-preview-gate") == 0) {
        a->preview_gate_off = 1;
      } else if (strcmp(flag, "--kotlinc-daemon") == 0) {
        a->kotlinc_daemon_on = 1;
      } else if (strcmp(flag, "--no-kotlinc-daemon") == 0) {
        a->kotlinc_daemon_off = 1;
      } else {
        return -1;
      }
    } else if (a->command == NULL) {
      a->command = arg;
    } else if (a->positional == NULL) {
      a->positional = arg;
    } else {
      return -1;
    }
  }
  return 0;
}

// ---------- lsp ----------

// `ccb lsp [--scenario NAME] [--trace FILE]`: run the preview LSP
// server on stdio for the project found by upward search. --scenario
// and --trace reuse the preview flags (parsed globally). Exit 0 on
// shutdown+exit, 1 on exit/EOF without shutdown, 2 on usage/config
// errors (unknown scenario, unreadable trace, no ccb.toml, preview
// not applicable). Other preview flags are rejected: they belong to
// the render modes, not to the server.
static int ccb_cmd_lsp(ccb_args *a) {
  char cwd[4096];
  char *path = NULL;
  char *err = NULL;
  pv_lsp_server *s = NULL;
  int rc = 0;
  if (a->preview_walk || a->preview_watch || a->preview_diff != NULL ||
      a->preview_storyboard != NULL || a->preview_screen != NULL ||
      a->preview_output != NULL || a->preview_update_goldens ||
      a->preview_record != NULL || a->preview_tree_live ||
      a->positional != NULL) {
    fprintf(stderr, "error: lsp takes only --scenario and --trace\n");
    return 2;
  }
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    fprintf(stderr, "error: cannot determine working directory\n");
    return 2;
  }
  if (ccb_config_discover(cwd, a->config_path, &path) != CCB_OK) {
    fprintf(stderr,
      "error: no ccb.toml found in this directory or any parent\n");
    return 2;
  }
  s = pv_lsp_open(path, a->preview_scenario, a->preview_trace, &err);
  free(path);
  if (s == NULL) {
    fprintf(stderr, "error: %s\n",
      err != NULL ? err : "cannot start lsp server");
    free(err);
    return 2;
  }
  rc = pv_lsp_run(s, STDIN_FILENO, STDOUT_FILENO);
  pv_lsp_close(s);
  return rc;
}

int ccb_cli_main(int argc, char **argv) {
  ccb_args a;
  if (ccb_parse_args(argc, argv, &a) != 0) {
    fputs(CCB_USAGE, stderr);
    return 2;
  }
  if (a.command == NULL) {
    fputs(CCB_USAGE, stderr);
    return 2;
  }
  if (strcmp(a.command, "init") == 0) {
    return ccb_cmd_init(&a);
  }
  if (strcmp(a.command, "setup") == 0) {
    return ccb_cmd_setup(&a);
  }
  if (strcmp(a.command, "check") == 0) {
    return ccb_cmd_check(&a);
  }
  if (strcmp(a.command, "build") == 0) {
    return ccb_cmd_build(&a);
  }
  if (strcmp(a.command, "graph") == 0) {
    return ccb_cmd_graph(&a);
  }
  if (strcmp(a.command, "clean") == 0) {
    return ccb_cmd_clean(&a);
  }
  if (strcmp(a.command, "daemon") == 0) {
    return ccb_cmd_daemon(&a);
  }
  if (strcmp(a.command, "doctor") == 0) {
    return ccb_cmd_doctor(&a);
  }
  if (strcmp(a.command, "install") == 0) {
    return ccb_cmd_install(&a);
  }
  if (strcmp(a.command, "run") == 0) {
    return ccb_cmd_run(&a);
  }
  if (strcmp(a.command, "logcat") == 0) {
    return ccb_cmd_logcat(&a);
  }
  if (strcmp(a.command, "preview") == 0) {
    return ccb_cmd_preview(&a);
  }
  if (strcmp(a.command, "lsp") == 0) {
    return ccb_cmd_lsp(&a);
  }
  if (strcmp(a.command, "version") == 0) {
    if (a.json) {
      printf("{\"event\":\"version\",\"version\":\"%s\"}\n", CCB_VERSION);
    } else {
      printf("ccb %s\n", CCB_VERSION);
    }
    return 0;
  }
  if (strcmp(a.command, "help") == 0) {
    fputs(CCB_USAGE, stdout);
    return 0;
  }
  fprintf(stderr, "error: unknown command '%s'\n", a.command);
  fputs(CCB_USAGE, stderr);
  return 2;
}
