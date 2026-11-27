// ccb_cli_internal.h: shared internals for the ccb_cli.c split:
// file-local types, macros, and the helpers called across the split
// files. Not public API: only the split files include this header.
#ifndef CCB_CLI_INTERNAL_H
#define CCB_CLI_INTERNAL_H

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
#include "ccb_graph.h"
#include "ccb_pipeline_graph.h"
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

#define CCB_CMDLINE_SHA256 "2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258"

#define CCB_KOTLIN_VERSION "2.4.21"

#define CCB_KOTLIN_URL "https://github.com/JetBrains/kotlin/releases/download/v2.4.21/kotlin-compiler-2.4.21.zip"

#define CCB_KOTLIN_SHA256 "7cc140e76daf416a0424a5557f99afd7ca89ebd463ea1101261e3e01c33e75cd"

#define CCB_R8_VERSION "9.5.28"

#define CCB_R8_URL "https://dl.google.com/dl/android/maven2/com/android/tools/r8/9.5.28/r8-9.5.28.jar"

#define CCB_R8_SHA256 "38a9ffaaebbd17d1514e3ecbff5a85b989bb13c551b9e224c7409514eea84ff5"

typedef struct {
  int json;
  int verbose;
  int dry_run;
  int explain;
  int yes;
  int upgrade;
  int print_env;
  int aab;
  const char *config_path;
  const char *variant;
  const char *command;
  const char *positional;
  const char *init_package;
  const char *init_name;
  int preview_ascii;
  int preview_strict;
  int preview_walk;
  int preview_watch;
  // Chunk 4: --tree-live (live logic-tree client, spawns ccb lsp).
  int preview_tree_live;
  long preview_width;
  int preview_width_set;
  const char *preview_device;
  const char *preview_screen;
  const char *preview_output;
  int preview_update_goldens;
  const char *preview_diff;
  const char *preview_storyboard;
  const char *preview_scenario;
  // Chunk 2: --trace FILE (render with the trace overlay) and
  // --record FILE (walk only: write the walk's trace).
  const char *preview_trace;
  const char *preview_record;
  int preview_gate_on;
  int preview_gate_off;
  int kotlinc_daemon_on;
  int kotlinc_daemon_off;
} ccb_args;

typedef struct {
  const char *rel;
  const char *content;
} ccb_scaffold_file;

typedef enum {
  CCB_JDKINST_NONE,
  CCB_JDKINST_TERMUX,
  CCB_JDKINST_APT,
  CCB_JDKINST_DNF,
  CCB_JDKINST_YUM,
  CCB_JDKINST_PACMAN,
  CCB_JDKINST_ZYPPER,
  CCB_JDKINST_APK
} ccb_jdkinst;

typedef struct {
  ccb_jdkinst kind;
  const char *display;  // "pkg", "apt-get", ... (string literal, not owned)
  char *path;           // resolved installer binary (owned)
  char *sudo_path;      // resolved sudo (owned, NULL unless needs_sudo)
  int needs_sudo;       // distro installer, not running as root
} ccb_jdkinst_info;

#define CCB_RENDIIX_LIST "etc/apt/sources.list.d/rendiix.list"

#define CCB_RENDIIX_LINE "deb https://rendiix.github.io android-tools termux"

#define CCB_RENDIIX_KEY_URL "https://rendiix.github.io/rendiix.gpg"

typedef struct {
  ccb_args *a;
  ccb_config *cfg;
  const char *sdk_home;
  char *default_home;
  char *sdkmanager;
  long compile_sdk;
  const char *build_tools;
  ccb_str platform;
  ccb_str bt;
} ccb_setup_ctx;

typedef struct {
  const char *tool;
  const char *pkg;
  const char *arg;
  int needs_repo;
  int needed;
} ccb_host_tool;

typedef struct {
  int exists;
  int has_classes;
  size_t res_files;
  size_t r_symbols;
  size_t assets;
} ccb_aar_summary;

#define CCB_PREVIEW_PHONE_WIDTH 40

#define CCB_PREVIEW_TABLET_WIDTH 80

typedef struct {
  ccb_args *a;
  char cwd[4096];
  char *toml_path;
  ccb_diag_list diags;
  int diags_owned;
  ccb_config *cfg;
  char *watch_toml;
  char *watch_config_dir;
  char *watch_styles;
  char *watch_flow;
  long eff_width;
} ccb_preview_ctx;

extern const char *CCB_USAGE;

void ccb_print_diags_human(ccb_diag_list *diags);
void ccb_print_diags(ccb_args *a, ccb_diag_list *diags);
ccb_config *ccb_load_config_or_die(ccb_args *a, ccb_diag_list *diags);
char *ccb_find_sdkmanager(const char *sdk_home);
int ccb_setup_missing_components(const char *sdk_home, long compile_sdk,
    const char *build_tools, const char **names);
int ccb_download_cached(const char *url, const char *dest, const char *expected_sha);
void ccb_detect_jdkinst(ccb_jdkinst_info *out);
int ccb_run_jdkinst_cmd(const ccb_jdkinst_info *inst, char *const cmd[]);
int ccb_install_jdk(const ccb_jdkinst_info *inst);
int ccb_prompt_yes(const char *question);
char *ccb_derive_java_home(const char *java_path);
void ccb_configure_java_home(const char *java_path);
int ccb_setup_probe_kotlinc(const ccb_config *cfg, const char *sdk_home,
    char **path_out, char **version_out);
int ccb_setup_install_kotlinc(const char *sdk_home);
int ccb_setup_install_r8(const char *sdk_home);
int ccb_setup_install_ndk(const char *sdk_home, const char *sdkmanager, const char *version);
int ccb_env_nonempty(const char *name);
int ccb_tool_runs(const char *path, const char *arg);
int ccb_path_tool_runs(const char *name, const char *arg);
int ccb_termux_repo_present(void);
int ccb_setup_install_system_tool(const ccb_jdkinst_info *inst, const char *pkg_name, int needs_repo);
int ccb_cmd_setup(ccb_args *a);
int ccb_cmd_check(ccb_args *a);
int ccb_cmd_build(ccb_args *a);
int ccb_cmd_graph(ccb_args *a);
int ccb_cmd_daemon(ccb_args *a);
int ccb_cmd_clean(ccb_args *a);
int ccb_cmd_doctor(ccb_args *a);
char *ccb_find_adb(ccb_config *cfg, ccb_diag_list *diags);
int ccb_cmd_install(ccb_args *a);
int ccb_cmd_run(ccb_args *a);
int ccb_cmd_logcat(ccb_args *a);
int ccb_preview_write_file(const char *path, const char *data,
    size_t len);
char *ccb_preview_golden_dir(const char *cwd);
int ccb_preview_update_goldens(pv_project *proj, int ascii,
    const char *golden_dir);
void ccb_preview_watch_paths_free(char *toml, char *dir,
    char *styles, char *flow);
int ccb_preview_render_once(ccb_args *a, pv_project *proj,
    pv_screen *screen, long errors);
int ccb_preview_watch_loop(ccb_args *a, const char *toml_path,
    const char *config_dir, const char *styles_dir, const char *flow_dir,
    long eff_width, pv_project *proj, long errors,
    const char *selected_name);
int ccb_cmd_preview(ccb_args *a);

#endif // CCB_CLI_INTERNAL_H
