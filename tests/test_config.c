// Tests for ccb_config: load, defaults, multi-error reporting, discovery.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_diag.h"
#include "ccb_config.h"

static const char *VALID =
  "ccb_version = 1\n"
  "[project]\n"
  "name = \"hello\"\n"
  "package = \"com.example.hello\"\n"
  "version = \"1.0\"\n"
  "min_sdk = 26\n"
  "target_sdk = 35\n"
  "compile_sdk = 35\n"
  "[paths]\n"
  "manifest = \"src/main/AndroidManifest.xml\"\n"
  "res = \"src/main/res\"\n"
  "kotlin = \"src/main/kotlin\"\n";

static int has_code(ccb_diag_list *d, const char *code) {
  for (size_t i = 0; i < d->count; i++) {
    if (strcmp(d->items[i].code, code) == 0) {
      return 1;
    }
  }
  return 0;
}

int main(void) {
  const char *base = "/tmp/ccb_test_cfg";
  ccb_remove_recursive(base);
  CHECK(ccb_mkdir_p("/tmp/ccb_test_cfg/proj/src/main/res") == CCB_OK);
  CHECK(ccb_mkdir_p("/tmp/ccb_test_cfg/proj/src/main/kotlin") == CCB_OK);
  CHECK(ccb_mkdir_p("/tmp/ccb_test_cfg/proj/sub/deep") == CCB_OK);
  CHECK(ccb_write_file("/tmp/ccb_test_cfg/proj/src/main/AndroidManifest.xml", "<manifest/>", 11) == CCB_OK);
  CHECK(ccb_write_file("/tmp/ccb_test_cfg/proj/ccb.toml", VALID, strlen(VALID)) == CCB_OK);

  // Valid load + defaults + absolute paths.
  {
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_config *c = NULL;
    CHECK(ccb_config_load("/tmp/ccb_test_cfg/proj/ccb.toml", NULL, &c, &diags) == CCB_OK);
    CHECK(diags.count == 0);
    CHECK(c != NULL);
    CHECK_STR_EQ(c->name, "hello");
    CHECK_STR_EQ(c->package, "com.example.hello");
    CHECK(c->version_code == 1);
    CHECK_STR_EQ(c->build_tools, "35.0.0");
    CHECK_STR_EQ(c->variant, "debug");
    CHECK_STR_EQ(c->jvm_target, "17");
    CHECK_STR_EQ(c->debug_alias, "androiddebugkey");
    CHECK_STR_EQ(c->manifest, "/tmp/ccb_test_cfg/proj/src/main/AndroidManifest.xml");
    CHECK_STR_EQ(c->build_dir, "/tmp/ccb_test_cfg/proj/build");
    CHECK_STR_EQ(c->project_root, "/tmp/ccb_test_cfg/proj");
    CHECK(c->has_release_signing == 0);
    ccb_config_free(c);
    ccb_diag_list_free(&diags);
  }

  // Discovery upward from a nested directory.
  {
    char *found = NULL;
    CHECK(ccb_config_discover("/tmp/ccb_test_cfg/proj/sub/deep", NULL, &found) == CCB_OK);
    CHECK_STR_EQ(found, "/tmp/ccb_test_cfg/proj/ccb.toml");
    free(found);
    CHECK(ccb_config_discover("/tmp/ccb_test_cfg/proj", "/tmp/ccb_test_cfg/nope.toml", &found) == CCB_ERR_CONFIG);
    CHECK(ccb_config_discover("/tmp", NULL, &found) == CCB_ERR_CONFIG);
  }

  // Invalid config: every problem reported at once.
  {
    const char *bad =
      "ccb_version = 1\n"
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.0\"\n"
      "min_sdk = \"26\"\n"
      "target_sdk = 35\n"
      "compile_sdk = 99\n"
      "surprise = true\n"
      "[paths]\n"
      "manifest = \"src/main/Missing.xml\"\n";
    CHECK(ccb_write_file("/tmp/ccb_test_cfg/bad.toml", bad, strlen(bad)) == CCB_OK);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_config *c = NULL;
    CHECK(ccb_config_load("/tmp/ccb_test_cfg/bad.toml", NULL, &c, &diags) == CCB_ERR_CONFIG);
    CHECK(c == NULL);
    CHECK(diags.count >= 5);
    CHECK(has_code(&diags, "CCB-E1001")); // surprise key
    CHECK(has_code(&diags, "CCB-E1004")); // missing package
    CHECK(has_code(&diags, "CCB-E1005")); // compile_sdk 99
    CHECK(has_code(&diags, "CCB-E1006")); // missing manifest file
    CHECK(has_code(&diags, "CCB-E1007")); // min_sdk wrong type
    ccb_diag_list_free(&diags);
  }

  // Release section requires all four fields; minify requires release.
  {
    const char *rel =
      "ccb_version = 1\n"
      "[project]\n"
      "name = \"x\"\n"
      "package = \"com.x\"\n"
      "version = \"1.0\"\n"
      "min_sdk = 26\n"
      "target_sdk = 35\n"
      "compile_sdk = 35\n"
      "[paths]\n"
      "manifest = \"src/main/AndroidManifest.xml\"\n"
      "[signing.release]\n"
      "store = \"rel.keystore\"\n"
      "[build]\n"
      "minify = true\n";
    CHECK(ccb_mkdir_p("/tmp/ccb_test_cfg/rel/src/main") == CCB_OK);
    CHECK(ccb_write_file("/tmp/ccb_test_cfg/rel/src/main/AndroidManifest.xml", "<manifest/>", 11) == CCB_OK);
    CHECK(ccb_write_file("/tmp/ccb_test_cfg/rel/ccb.toml", rel, strlen(rel)) == CCB_OK);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_config *c = NULL;
    CHECK(ccb_config_load("/tmp/ccb_test_cfg/rel/ccb.toml", NULL, &c, &diags) == CCB_ERR_CONFIG);
    CHECK(has_code(&diags, "CCB-E1004")); // missing alias/envs (+ proguard)
    CHECK(has_code(&diags, "CCB-E1005")); // minify on debug
    CHECK(has_code(&diags, "CCB-E1006")); // missing keystore file
    ccb_diag_list_free(&diags);
  }

  // The --variant override applies before minify validation: the same
  // TOML loads fine with a "release" override.
  {
    const char *rel =
      "ccb_version = 1\n"
      "[project]\n"
      "name = \"x\"\n"
      "package = \"com.x\"\n"
      "version = \"1.0\"\n"
      "min_sdk = 26\n"
      "target_sdk = 35\n"
      "compile_sdk = 35\n"
      "[paths]\n"
      "manifest = \"src/main/AndroidManifest.xml\"\n"
      "[build]\n"
      "minify = true\n"
      "proguard_rules = \"rules.pro\"\n";
    CHECK(ccb_mkdir_p("/tmp/ccb_test_cfg/ovr/src/main") == CCB_OK);
    CHECK(ccb_write_file("/tmp/ccb_test_cfg/ovr/src/main/AndroidManifest.xml", "<manifest/>", 11) == CCB_OK);
    CHECK(ccb_write_file("/tmp/ccb_test_cfg/ovr/rules.pro", "# keep\n", 7) == CCB_OK);
    CHECK(ccb_write_file("/tmp/ccb_test_cfg/ovr/ccb.toml", rel, strlen(rel)) == CCB_OK);
    ccb_diag_list diags;
    ccb_diag_list_init(&diags);
    ccb_config *c = NULL;
    CHECK(ccb_config_load("/tmp/ccb_test_cfg/ovr/ccb.toml", NULL, &c, &diags) == CCB_ERR_CONFIG);
    CHECK(has_code(&diags, "CCB-E1005")); // minify on debug
    ccb_diag_list_free(&diags);
    ccb_diag_list_init(&diags);
    c = NULL;
    CHECK(ccb_config_load("/tmp/ccb_test_cfg/ovr/ccb.toml", "release", &c, &diags) == CCB_OK);
    CHECK_STR_EQ(c->variant, "release");
    ccb_config_free(c);
    ccb_diag_list_free(&diags);
  }

  ccb_remove_recursive(base);
  TEST_MAIN_END();
}
