// Tests for @string/@color resolution via [preview] resources.
// The checked-in fixture lives under tests/fixtures/preview/
// resources/; scratch projects (mkdtemp) cover the missing-file,
// malformed-entry, and entity cases. All paths derive from __FILE__
// for the fixture; scratch tests never depend on the working dir.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_preview.h"
#include "ccb_config.h"
#include "ccb_str.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_dir(ccb_str *out) {
  const char *file = __FILE__;
  const char *slash = strrchr(file, '/');
  if (slash != NULL && slash != file) {
    ccb_str_append_n(out, file, (size_t)(slash - file));
  } else {
    ccb_str_append(out, ".");
  }
}

static int write_all(const char *path, const char *data) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return -1;
  }
  if (fwrite(data, 1, strlen(data), f) != strlen(data)) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static char *join2(const char *a, const char *b) {
  ccb_str s;
  char *out = NULL;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  ccb_str_append(&s, "/");
  ccb_str_append(&s, b);
  out = ccb_xstrdup(s.buf);
  ccb_str_free(&s);
  return out;
}

static char *fixture_toml_path(void) {
  ccb_str dir;
  ccb_str path;
  FILE *f = NULL;
  ccb_str_init(&dir);
  test_dir(&dir);
  ccb_str_init(&path);
  ccb_str_append(&path, dir.buf != NULL ? dir.buf : ".");
  ccb_str_append(&path, "/fixtures/preview/resources/ccb.toml");
  f = fopen(path.buf != NULL ? path.buf : "", "rb");
  if (f != NULL) {
    fclose(f);
    {
      char *out = ccb_xstrdup(path.buf);
      ccb_str_free(&path);
      ccb_str_free(&dir);
      return out;
    }
  }
  ccb_str_free(&path);
  ccb_str_free(&dir);
  return ccb_xstrdup("tests/fixtures/preview/resources/ccb.toml");
}

static pv_project *load_project(const char *toml_path) {
  ccb_diag_list diags;
  ccb_config *cfg = NULL;
  pv_project *p = NULL;
  ccb_diag_list_init(&diags);
  if (ccb_config_load(toml_path, NULL, &cfg, &diags) != CCB_OK) {
    size_t i = 0;
    fprintf(stderr, "config load failed for %s:\n", toml_path);
    for (i = 0; i < diags.count; i++) {
      fprintf(stderr, "  %s: %s\n", diags.items[i].code,
        diags.items[i].message);
    }
    ccb_diag_list_free(&diags);
    return NULL;
  }
  ccb_diag_list_free(&diags);
  if (pv_project_load(cfg, 40, 0, 0, NULL, &p) != CCB_OK) {
    ccb_config_free(cfg);
    return NULL;
  }
  ccb_config_free(cfg);
  return p;
}

static int diag_count(pv_project *p, const char *code) {
  size_t i = 0;
  int n = 0;
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0) {
      n++;
    }
  }
  return n;
}

static const pv_rule *find_rule(pv_project *p, const char *sel) {
  size_t i = 0;
  for (i = 0; i < p->nrules; i++) {
    if (p->rules[i].selector != NULL &&
        strcmp(p->rules[i].selector, sel) == 0) {
      return &p->rules[i];
    }
  }
  return NULL;
}

static pv_screen *find_screen(pv_project *p, const char *name) {
  size_t i = 0;
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
}

// Build a scratch project: flow + styles + res files as given
// (strings_xml / colors_xml NULL means the file is not written).
// Returns the malloc'd project dir, or NULL.
static char *make_scratch(const char *flow, const char *css,
    const char *strings_xml, const char *colors_xml) {
  char buf[64];
  ccb_test_tmpdir(buf, sizeof buf, "res");
  char *sub = NULL;
  char *path = NULL;
  ccb_str toml;
  char *dir = ccb_xstrdup(buf);
  sub = join2(dir, "flow");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "styles");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "res");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "src");
  mkdir(sub, 0777);
  free(sub);
  sub = join2(dir, "src/main");
  mkdir(sub, 0777);
  free(sub);
  ccb_str_init(&toml);
  ccb_str_append(&toml,
    "ccb_version = 1\n"
    "[project]\n"
    "name = \"scratch\"\n"
    "package = \"com.example.scratch\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "[preview]\n"
    "styles = \"styles\"\n"
    "flow = \"flow\"\n"
    "resources = \"res\"\n"
    "phone_width = 390\n"
    "phone_height = 844\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n");
  path = join2(dir, "ccb.toml");
  if (write_all(path, toml.buf) != 0) {
    free(path);
    ccb_str_free(&toml);
    return dir;
  }
  free(path);
  ccb_str_free(&toml);
  path = join2(dir, "src/main/AndroidManifest.xml");
  if (write_all(path, "<manifest />\n") != 0) {
    free(path);
    return dir;
  }
  free(path);
  path = join2(dir, "flow/Home.kt");
  if (write_all(path, flow) != 0) {
    free(path);
    return dir;
  }
  free(path);
  path = join2(dir, "styles/common.css");
  if (write_all(path, css) != 0) {
    free(path);
    return dir;
  }
  free(path);
  if (strings_xml != NULL) {
    path = join2(dir, "res/strings.xml");
    if (write_all(path, strings_xml) != 0) {
      free(path);
      return dir;
    }
    free(path);
  }
  if (colors_xml != NULL) {
    path = join2(dir, "res/colors.xml");
    if (write_all(path, colors_xml) != 0) {
      free(path);
      return dir;
    }
    free(path);
  }
  return dir;
}

int main(void) {
  // --- Checked-in fixture: string + color resolution, unresolved
  // references kept literally with CCB-P1016, malformed entries
  // skipped with CCB-P1016, comment entries not parsed.
  {
    char *toml = fixture_toml_path();
    pv_project *p = load_project(toml);
    pv_screen *home = NULL;
    pv_box *col = NULL;
    const pv_rule *screen_rule = NULL;
    const pv_rule *text_rule = NULL;
    const pv_rule *button_rule = NULL;
    ccb_str out;
    long errors = 0;
    CHECK(p != NULL);
    if (p != NULL) {
      errors = pv_project_diagnose(p);
      CHECK(errors == 0);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      if (home != NULL && home->root != NULL &&
          home->root->nchildren == 1) {
        col = home->root->children[0];
        CHECK(col->nchildren == 3);
        if (col->nchildren == 3) {
          // Resolved + entity-decoded.
          CHECK_STR_EQ(col->children[0]->label, "My Notes & More");
          // Unresolved: kept literally.
          CHECK_STR_EQ(col->children[1]->label, "@string/missing");
        }
      }
      screen_rule = find_rule(p, "screen");
      text_rule = find_rule(p, "text");
      button_rule = find_rule(p, "button");
      CHECK(screen_rule != NULL);
      if (screen_rule != NULL) {
        CHECK_STR_EQ(screen_rule->bg, "#ffffff");
      }
      CHECK(text_rule != NULL);
      if (text_rule != NULL) {
        CHECK_STR_EQ(text_rule->fg, "#111111");
      }
      CHECK(button_rule != NULL);
      if (button_rule != NULL) {
        // Unresolved @color: kept literally, never invented.
        CHECK_STR_EQ(button_rule->bg, "@color/missing");
      }
      // One P1016 each: unresolved string, unresolved color,
      // malformed string entry, non-hex color entry.
      CHECK(diag_count(p, "CCB-P1016") == 4);
      ccb_str_init(&out);
      if (home != NULL) {
        pv_render_screen(&out, home, p->frame_width, 0, 0);
        CHECK(ccb_test_contains(out.buf, "My Notes & More"));
        CHECK(ccb_test_contains(out.buf, "@string/missing"));
        CHECK(!ccb_test_contains(out.buf, "Nope"));
      }
      ccb_str_free(&out);
      pv_project_free(p);
    }
    free(toml);
  }
  // --- Missing colors.xml file: no diagnostic, strings still work.
  {
    char *dir = make_scratch(
      "fun Home() { Column { Text(\"@string/hi\") Text(\"plain\") } }\n",
      "screen { padding: 0px; }\n",
      "<resources><string name=\"hi\">Hi</string></resources>\n",
      NULL);
    char *toml = NULL;
    pv_project *p = NULL;
    pv_screen *home = NULL;
    CHECK(dir != NULL);
    toml = join2(dir, "ccb.toml");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(diag_count(p, "CCB-P1016") == 0);
      home = find_screen(p, "Home");
      if (home != NULL && home->root->nchildren == 1 &&
          home->root->children[0]->nchildren == 2) {
        CHECK_STR_EQ(home->root->children[0]->children[0]->label, "Hi");
        CHECK_STR_EQ(home->root->children[0]->children[1]->label,
          "plain");
      }
      pv_project_free(p);
    }
    free(toml);
    free(dir);
  }
  // --- Empty resources dir: the reference itself warns (P1016),
  // but the missing files add no diagnostic of their own.
  {
    char *dir = make_scratch(
      "fun Home() { Column { Text(\"@string/x\") } }\n",
      "screen { padding: 0px; }\n",
      NULL, NULL);
    char *toml = join2(dir, "ccb.toml");
    pv_project *p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(diag_count(p, "CCB-P1016") == 1);
      pv_project_free(p);
    }
    free(toml);
    free(dir);
  }
  // --- Malformed entry (no name) is skipped with P1016; the valid
  // entry after it still resolves.
  {
    char *dir = make_scratch(
      "fun Home() { Column { Text(\"@string/ok\") } }\n",
      "screen { padding: 0px; }\n",
      "<resources><string>nameless</string>"
      "<string name=\"ok\">Fine</string></resources>\n",
      NULL);
    char *toml = join2(dir, "ccb.toml");
    pv_project *p = load_project(toml);
    pv_screen *home = NULL;
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(diag_count(p, "CCB-P1016") == 1);
      home = find_screen(p, "Home");
      if (home != NULL && home->root->nchildren == 1 &&
          home->root->children[0]->nchildren == 1) {
        CHECK_STR_EQ(home->root->children[0]->children[0]->label,
          "Fine");
      }
      pv_project_free(p);
    }
    free(toml);
    free(dir);
  }
  // --- Entity decoding in string values: all five entities.
  {
    char *dir = make_scratch(
      "fun Home() { Column { Text(\"@string/e\") } }\n",
      "screen { padding: 0px; }\n",
      "<resources><string name=\"e\">A &lt; B &gt; C &quot;Q&quot; "
      "&apos;S&apos; &amp; D</string></resources>\n",
      NULL);
    char *toml = join2(dir, "ccb.toml");
    pv_project *p = load_project(toml);
    pv_screen *home = NULL;
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(diag_count(p, "CCB-P1016") == 0);
      home = find_screen(p, "Home");
      if (home != NULL && home->root->nchildren == 1 &&
          home->root->children[0]->nchildren == 1) {
        CHECK_STR_EQ(home->root->children[0]->children[0]->label,
          "A < B > C \"Q\" 'S' & D");
      }
      pv_project_free(p);
    }
    free(toml);
    free(dir);
  }
  // --- A @color-resolved background feeds the contrast check: the
  // same gray-on-white text fails at the default threshold, and
  // passes as large text (font-size 24px, 3.0:1 threshold).
  {
    char *dir = make_scratch(
      "fun Home() { Column { Text(\"Gray\") } }\n",
      "screen { background-color: @color/bg; }\n"
      "text { color: #888888; }\n",
      NULL,
      "<resources><color name=\"bg\">#ffffff</color></resources>\n");
    char *toml = join2(dir, "ccb.toml");
    pv_project *p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(diag_count(p, "CCB-P1018") == 1);
      pv_project_free(p);
    }
    free(toml);
    free(dir);
  }
  {
    char *dir = make_scratch(
      "fun Home() { Column { Text(\"Gray\") } }\n",
      "screen { background-color: @color/bg; }\n"
      "text { color: #888888; font-size: 24px; }\n",
      NULL,
      "<resources><color name=\"bg\">#ffffff</color></resources>\n");
    char *toml = join2(dir, "ccb.toml");
    pv_project *p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(diag_count(p, "CCB-P1018") == 0);
      pv_project_free(p);
    }
    free(toml);
    free(dir);
  }
  TEST_MAIN_END();
}
