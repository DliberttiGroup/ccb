// Tests for ccb_toml.
#include "test.h"
#include "ccb_toml.h"

static const char *GOOD =
  "# comment\n"
  "ccb_version = 1\n"
  "\n"
  "[project]\n"
  "name = \"hello\"\n"
  "package = 'com.example.hello'\n"
  "version = \"1.0\"\n"
  "version_code = 1_000\n"
  "min_sdk = 26\n"
  "target_sdk = 35\n"
  "compile_sdk = 35\n"
  "\n"
  "[paths]\n"
  "manifest = \"src/main/AndroidManifest.xml\"\n"
  "kotlin = \"src/main/kotlin\"\n"
  "\n"
  "[deps]\n"
  "jars = [\n"
  "  \"libs/a.jar\", # a comment with ] bracket\n"
  "  'libs/b.jar',\n"
  "]\n"
  "\n"
  "[build]\n"
  "variant = \"debug\"\n"
  "minify = false\n";

int main(void) {
  // Happy path.
  {
    ccb_toml_error *errors = NULL;
    size_t nerrors = 0;
    ccb_toml_doc *doc = ccb_toml_parse(GOOD, &errors, &nerrors);
    CHECK(doc != NULL);
    CHECK(nerrors == 0);
    const ccb_toml_table *root = ccb_toml_find_table(doc, "");
    CHECK(root != NULL);
    const ccb_toml_value *v = ccb_toml_find_value(root, "ccb_version");
    CHECK(v != NULL && v->type == CCB_TOML_INT && v->integer == 1);
    const ccb_toml_table *proj = ccb_toml_find_table(doc, "project");
    CHECK(proj != NULL);
    const ccb_toml_value *name = ccb_toml_find_value(proj, "name");
    CHECK(name != NULL && name->type == CCB_TOML_STRING);
    CHECK_STR_EQ(name->str, "hello");
    const ccb_toml_value *pkg = ccb_toml_find_value(proj, "package");
    CHECK_STR_EQ(pkg->str, "com.example.hello");
    const ccb_toml_value *vc = ccb_toml_find_value(proj, "version_code");
    CHECK(vc != NULL && vc->integer == 1000);
    const ccb_toml_table *deps = ccb_toml_find_table(doc, "deps");
    const ccb_toml_value *jars = ccb_toml_find_value(deps, "jars");
    CHECK(jars != NULL && jars->type == CCB_TOML_STR_ARRAY && jars->count == 2);
    CHECK_STR_EQ(jars->items[0], "libs/a.jar");
    CHECK_STR_EQ(jars->items[1], "libs/b.jar");
    const ccb_toml_table *build = ccb_toml_find_table(doc, "build");
    const ccb_toml_value *minify = ccb_toml_find_value(build, "minify");
    CHECK(minify != NULL && minify->type == CCB_TOML_BOOL && minify->boolean == 0);
    CHECK(ccb_toml_find_table(doc, "missing") == NULL);
    CHECK(ccb_toml_find_value(proj, "missing") == NULL);
    ccb_toml_free_doc(doc);
  }
  // String escapes.
  {
    ccb_toml_doc *doc = ccb_toml_parse("k = \"a\\n\\t\\\"b\\u0041\"", NULL, NULL);
    CHECK(doc != NULL);
    const ccb_toml_value *v = ccb_toml_find_value(ccb_toml_find_table(doc, ""), "k");
    CHECK_STR_EQ(v->str, "a\n\t\"bA");
    ccb_toml_free_doc(doc);
  }
  // Multiple errors reported at once.
  {
    const char *bad =
      "[project]\n"
      "name = \"x\"\n"
      "name = \"y\"\n"
      "version = 1.5\n"
      "broken = \"unterminated\n"
      "[project]\n"
      "other = 1\n";
    ccb_toml_error *errors = NULL;
    size_t nerrors = 0;
    ccb_toml_doc *doc = ccb_toml_parse(bad, &errors, &nerrors);
    CHECK(doc == NULL);
    CHECK(nerrors >= 4);
    for (size_t i = 0; i < nerrors; i++) {
      CHECK(errors[i].line >= 1);
      CHECK(errors[i].message != NULL && errors[i].message[0] != '\0');
    }
    ccb_toml_free_errors(errors, nerrors);
  }
  // Non-string array element rejected.
  {
    ccb_toml_doc *doc = ccb_toml_parse("a = [1, 2]\n", NULL, NULL);
    CHECK(doc == NULL);
  }
  // Dotted keys rejected.
  {
    ccb_toml_doc *doc = ccb_toml_parse("a.b = 1\n", NULL, NULL);
    CHECK(doc == NULL);
  }
  // Trailing junk rejected.
  {
    ccb_toml_doc *doc = ccb_toml_parse("a = 1 junk\n", NULL, NULL);
    CHECK(doc == NULL);
  }
  TEST_MAIN_END();
}
