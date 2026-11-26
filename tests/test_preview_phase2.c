// Tests for Phase 2 (real-code front end): Activities + layout XML
// parsed into screens/edges, merged with the flow sketch (or
// standing alone when there is no flow/), the manifest launcher as
// the entry screen, helper call-site args, and CCB-P1025 for navs
// no parse attributes. All paths derive from __FILE__; scratch
// tests never depend on the working directory.
#include "ccb_common.h"
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_cli.h"
#include "ccb_config.h"
#include "ccb_preview.h"
#include <stdio.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- helpers (same shapes as the Chunk B/C tests) ----------

static char *test_dir(void) {
  const char *f = __FILE__;
  const char *slash = strrchr(f, '/');
  size_t n = 0;
  char *out = NULL;
  if (slash == NULL) {
    return ccb_xstrdup(".");
  }
  n = (size_t)(slash - f);
  out = ccb_xmalloc(n + 1);
  memcpy(out, f, n);
  out[n] = '\0';
  return out;
}

static char *join2(const char *a, const char *b) {
  ccb_str s;
  char *out = NULL;
  ccb_str_init(&s);
  ccb_str_append(&s, a);
  ccb_str_append(&s, "/");
  ccb_str_append(&s, b);
  out = ccb_xstrdup(s.buf != NULL ? s.buf : "");
  ccb_str_free(&s);
  return out;
}

static void mkdir_one(const char *path) {
  (void)mkdir(path, 0777);
}

static int write_all(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    return -1;
  }
  fputs(text, f);
  fclose(f);
  return 0;
}

static int write_joined(const char *dir, const char *name,
    const char *text) {
  char *path = join2(dir, name);
  int rc = write_all(path, text);
  free(path);
  return rc;
}

static char *try_read(const char *path) {
  FILE *f = fopen(path, "r");
  ccb_str s;
  char buf[4096];
  size_t n = 0;
  if (f == NULL) {
    return NULL;
  }
  ccb_str_init(&s);
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    ccb_str_append_n(&s, buf, n);
  }
  fclose(f);
  return s.buf;
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

static int diag_message_contains(pv_project *p, const char *code,
    const char *needle) {
  size_t i = 0;
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0 &&
        p->diags.items[i].message != NULL &&
        strstr(p->diags.items[i].message, needle) != NULL) {
      return 1;
    }
  }
  return 0;
}

// Line of the first diagnostic with the given code whose message
// contains needle, or -1 when there is none.
static long diag_line(pv_project *p, const char *code,
    const char *needle) {
  size_t i = 0;
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0 &&
        p->diags.items[i].message != NULL &&
        strstr(p->diags.items[i].message, needle) != NULL) {
      return p->diags.items[i].line;
    }
  }
  return -1;
}

// File of the first diagnostic with the given code whose message
// contains needle, or NULL when there is none.
static const char *diag_file(pv_project *p, const char *code,
    const char *needle) {
  size_t i = 0;
  for (i = 0; i < p->diags.count; i++) {
    if (strcmp(p->diags.items[i].code, code) == 0 &&
        p->diags.items[i].message != NULL &&
        strstr(p->diags.items[i].message, needle) != NULL) {
      return p->diags.items[i].file;
    }
  }
  return NULL;
}

static pv_screen *find_screen(pv_project *p, const char *name) {
  size_t i = 0;
  if (p == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0; i < p->nscreens; i++) {
    if (p->screens[i]->name != NULL &&
        strcmp(p->screens[i]->name, name) == 0) {
      return p->screens[i];
    }
  }
  return NULL;
}

static pv_edge *find_edge(pv_screen *screen, const char *from) {
  size_t i = 0;
  if (screen == NULL || from == NULL) {
    return NULL;
  }
  for (i = 0; i < screen->nedges; i++) {
    if (screen->edges[i].from != NULL &&
        strcmp(screen->edges[i].from, from) == 0) {
      return &screen->edges[i];
    }
  }
  return NULL;
}

// Build a scratch real-code project: manifest with MainActivity as
// the launcher, [paths] kotlin + res, [preview] layout/nav_graph/
// resources (flow/ too when has_flow). Returns the project dir;
// tests write the .kt/.xml/flow files into it themselves.
static char *mk_real_proj(int has_flow) {
  char scratch[64];
  ccb_test_tmpdir(scratch, sizeof scratch, "p2");
  char *proj = NULL;
  char *src = NULL;
  char *main = NULL;
  char *kt = NULL;
  char *pkg = NULL;
  char *res = NULL;
  char *layout = NULL;
  char *nav = NULL;
  char *values = NULL;
  char *man = NULL;
  char *toml = NULL;
  ccb_str s;
  proj = join2(scratch, "proj");
  src = join2(proj, "src");
  main = join2(src, "main");
  kt = join2(main, "kotlin");
  pkg = join2(kt, "com/example/app");
  res = join2(main, "res");
  layout = join2(res, "layout");
  nav = join2(res, "navigation");
  values = join2(res, "values");
  man = join2(main, "AndroidManifest.xml");
  toml = join2(proj, "ccb.toml");
  mkdir_one(proj);
  mkdir_one(src);
  mkdir_one(main);
  mkdir_one(kt);
  {
    char *com = join2(kt, "com");
    char *comex = join2(kt, "com/example");
    mkdir_one(com);
    mkdir_one(comex);
    free(com);
    free(comex);
  }
  mkdir_one(pkg);
  mkdir_one(res);
  mkdir_one(layout);
  mkdir_one(nav);
  mkdir_one(values);
  if (has_flow) {
    char *flow = join2(proj, "flow");
    mkdir_one(flow);
    free(flow);
  }
  write_all(man,
    "<manifest xmlns:android="
    "\"http://schemas.android.com/apk/res/android\"\n"
    "  package=\"com.example.app\">\n"
    "  <application>\n"
    "    <activity android:name=\".MainActivity\">\n"
    "      <intent-filter>\n"
    "        <action android:name=\"android.intent.action.MAIN\" />\n"
    "        <category android:name="
    "\"android.intent.category.LAUNCHER\" />\n"
    "      </intent-filter>\n"
    "    </activity>\n"
    "    <activity android:name=\".DetailActivity\" />\n"
    "    <activity android:name=\".FormActivity\" />\n"
    "    <activity android:name=\".SettingsActivity\" />\n"
    "  </application>\n"
    "</manifest>\n");
  write_joined(values, "strings.xml", "<resources>\n</resources>\n");
  ccb_str_init(&s);
  ccb_str_append(&s,
    "ccb_version = 1\n"
    "[project]\n"
    "name = \"phase2\"\n"
    "package = \"com.example.app\"\n"
    "version = \"1.0\"\n"
    "version_code = 1\n"
    "min_sdk = 26\n"
    "target_sdk = 35\n"
    "compile_sdk = 35\n"
    "[paths]\n"
    "manifest = \"src/main/AndroidManifest.xml\"\n"
    "res = \"src/main/res\"\n"
    "kotlin = \"src/main/kotlin\"\n"
    "[preview]\n"
    "layout = \"src/main/res/layout\"\n"
    "nav_graph = \"src/main/res/navigation\"\n"
    "resources = \"src/main/res/values\"\n"
    "phone_width = 390\n"
    "phone_height = 844\n");
  if (has_flow) {
    ccb_str_append(&s, "flow = \"flow\"\n");
  }
  write_all(toml, s.buf != NULL ? s.buf : "");
  ccb_str_free(&s);
  free(src);
  free(main);
  free(kt);
  free(pkg);
  free(res);
  free(layout);
  free(nav);
  free(values);
  free(man);
  free(toml);
  return proj;
}

static char *kt_dir(char *proj) {
  char *a = join2(proj, "src/main/kotlin/com/example/app");
  return a;
}

static char *layout_dir(char *proj) {
  return join2(proj, "src/main/res/layout");
}

static char *nav_dir(char *proj) {
  return join2(proj, "src/main/res/navigation");
}

static char *values_dir(char *proj) {
  return join2(proj, "src/main/res/values");
}

// Child box at index, or NULL.
static pv_box *box_child(pv_box *box, size_t idx) {
  if (box == NULL || idx >= box->nchildren) {
    return NULL;
  }
  return box->children[idx];
}

// Recursively copy a directory tree.
static int copy_tree(const char *src, const char *dst) {
  DIR *d = opendir(src);
  struct dirent *e = NULL;
  if (d == NULL) {
    return -1;
  }
  mkdir_one(dst);
  while ((e = readdir(d)) != NULL) {
    char *sp = NULL;
    char *dp = NULL;
    struct stat st;
    int rc = 0;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    sp = join2(src, e->d_name);
    dp = join2(dst, e->d_name);
    if (stat(sp, &st) != 0) {
      free(sp);
      free(dp);
      closedir(d);
      return -1;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = copy_tree(sp, dp);
    } else {
      char *text = try_read(sp);
      rc = text == NULL ? -1 : write_all(dp, text);
      free(text);
    }
    free(sp);
    free(dp);
    if (rc != 0) {
      closedir(d);
      return -1;
    }
  }
  closedir(d);
  return 0;
}

// Golden files live under tests/golden/preview/real-world/.
static char *read_golden(const char *rel) {
  static const char *suffixes[] = {
    "/golden/preview/",
    "/../tests/golden/preview/",
  };
  size_t si = 0;
  for (si = 0; si < 2; si++) {
    char *td = test_dir();
    ccb_str path;
    char *out = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, td);
    ccb_str_append(&path, suffixes[si]);
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
    free(td);
    if (out != NULL) {
      return out;
    }
  }
  {
    ccb_str path;
    char *out = NULL;
    ccb_str_init(&path);
    ccb_str_append(&path, "tests/golden/preview/");
    ccb_str_append(&path, rel);
    out = try_read(path.buf != NULL ? path.buf : "");
    ccb_str_free(&path);
    if (out != NULL) {
      return out;
    }
  }
  return NULL;
}

// Replace every occurrence of `from` in `text` with `to`.
// Returned string is malloc'd via ccb_str (caller frees it).
static char *replace_all(const char *text, const char *from,
    const char *to) {
  ccb_str out;
  const char *p = NULL;
  size_t flen = 0;
  if (text == NULL || from == NULL || from[0] == '\0' ||
      to == NULL) {
    return ccb_xstrdup(text != NULL ? text : "");
  }
  flen = strlen(from);
  ccb_str_init(&out);
  p = text;
  while (*p != '\0') {
    const char *hit = strstr(p, from);
    if (hit == NULL) {
      ccb_str_append(&out, p);
      break;
    }
    ccb_str_append_n(&out, p, (size_t)(hit - p));
    ccb_str_append(&out, to);
    p = hit + flen;
  }
  return out.buf != NULL ? out.buf : ccb_xstrdup("");
}

static void check_golden_file(const char *label, const char *actual,
    const char *golden_rel) {
  char *expected = read_golden(golden_rel);
  CHECK(expected != NULL);
  if (expected == NULL) {
    fprintf(stderr, "FAIL golden %s: cannot read %s\n", label,
      golden_rel);
    return;
  }
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "FAIL golden %s: mismatch with %s\n", label,
      golden_rel);
  }
  CHECK(strcmp(actual, expected) == 0);
  free(expected);
}

int main(void) {
  // (1) No-sketch P1-style project: screens, entry = launcher,
  // helper args (literal kept, variable -> unknown arg), onClick
  // via layout text with no config binding, Intent-val shape.
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    pv_screen *detail = NULL;
    pv_edge *e = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.content.Intent\n"
      "import android.os.Bundle\n"
      "import android.view.View\n"
      "import android.widget.Button\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "    findViewById<Button>(R.id.note1).setOnClickListener {\n"
      "      openDetail(\"note-1\")\n"
      "    }\n"
      "    findViewById<Button>(R.id.note2).setOnClickListener {\n"
      "      openDetail(\"note-2\")\n"
      "    }\n"
      "    findViewById<Button>(R.id.note3).setOnClickListener {\n"
      "      openDetail(\"note-3\")\n"
      "    }\n"
      "    findViewById<Button>(R.id.new_note).setOnClickListener {\n"
      "      startActivity(Intent(this, FormActivity::class.java))\n"
      "    }\n"
      "    findViewById<Button>(R.id.quick).setOnClickListener {\n"
      "      openDetail(selectedId)\n"
      "    }\n"
      "  }\n"
      "\n"
      "  fun onSettingsClick(v: View) {\n"
      "    startActivity(Intent(this, SettingsActivity::class.java))\n"
      "  }\n"
      "\n"
      "  private fun openDetail(noteId: String) {\n"
      "    val intent = Intent(this, DetailActivity::class.java)\n"
      "    intent.putExtra(\"noteId\", noteId)\n"
      "    startActivity(intent)\n"
      "  }\n"
      "}\n");
    write_joined(kd, "DetailActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.content.Intent\n"
      "import android.os.Bundle\n"
      "import android.widget.Button\n"
      "\n"
      "class DetailActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_detail)\n"
      "    findViewById<Button>(R.id.edit).setOnClickListener {\n"
      "      val intent = Intent(this, FormActivity::class.java)\n"
      "      startActivity(intent)\n"
      "    }\n"
      "  }\n"
      "}\n");
    write_joined(kd, "FormActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class FormActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_form)\n"
      "  }\n"
      "}\n");
    write_joined(kd, "SettingsActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class SettingsActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_settings)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <Button android:id=\"@+id/note1\" "
      "android:text=\"Note 1: Buy milk\" />\n"
      "  <Button android:id=\"@+id/note2\" "
      "android:text=\"Note 2: Call Ana\" />\n"
      "  <Button android:id=\"@+id/note3\" "
      "android:text=\"Note 3: Ideas\" />\n"
      "  <Button android:id=\"@+id/new_note\" "
      "android:text=\"New note\" />\n"
      "  <Button android:id=\"@+id/quick\" "
      "android:text=\"Quick open\" />\n"
      "  <Button android:id=\"@+id/settings_button\" "
      "android:text=\"Settings\" "
      "android:onClick=\"onSettingsClick\" />\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_detail.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <Button android:id=\"@+id/edit\" android:text=\"Edit\" />\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_form.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_settings.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      (void)pv_project_diagnose(p);
      CHECK(p->nscreens == 4);
      CHECK(p->screens[0] != NULL &&
        strcmp(p->screens[0]->name, "Main") == 0);
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      CHECK(find_screen(p, "Detail") != NULL);
      CHECK(find_screen(p, "Form") != NULL);
      CHECK(find_screen(p, "Settings") != NULL);
      e = find_edge(main_screen, "Note 1: Buy milk");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
        CHECK(e->nargs == 1);
        CHECK(e->args != NULL && e->args[0] != NULL &&
          strcmp(e->args[0], "note-1") == 0);
      }
      e = find_edge(main_screen, "Note 2: Call Ana");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
        CHECK(e->nargs == 1);
        CHECK(e->args != NULL && e->args[0] != NULL &&
          strcmp(e->args[0], "note-2") == 0);
      }
      e = find_edge(main_screen, "Note 3: Ideas");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
        CHECK(e->nargs == 1);
        CHECK(e->args != NULL && e->args[0] != NULL &&
          strcmp(e->args[0], "note-3") == 0);
      }
      e = find_edge(main_screen, "New note");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Form");
        CHECK(e->nargs == 0);
      }
      e = find_edge(main_screen, "Quick open");
      CHECK(e != NULL);
      if (e != NULL) {
        // Call-site variable argument: the helper's nav is
        // recorded, with an unknown argument.
        CHECK_STR_EQ(e->target, "Detail");
        CHECK(e->nargs == 1);
        CHECK(e->args != NULL && e->args[0] == NULL);
      }
      e = find_edge(main_screen, "Settings");
      CHECK(e != NULL);
      if (e != NULL) {
        // android:onClick, bound by the layout's own text: no
        // [preview.bindings] entry was configured.
        CHECK_STR_EQ(e->target, "Settings");
      }
      detail = find_screen(p, "Detail");
      e = find_edge(detail, "Edit");
      CHECK(e != NULL);
      if (e != NULL) {
        // Intent-val shape: val intent = Intent(...); startActivity.
        CHECK_STR_EQ(e->target, "Form");
      }
      CHECK(diag_count(p, "CCB-P1005") == 0);
      CHECK(diag_count(p, "CCB-P1019") == 0);
      CHECK(diag_count(p, "CCB-P1020") == 0);
      CHECK(diag_count(p, "CCB-P1025") == 0);
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(toml);
    free(proj);
  }

  // (2) Merge with a sketch: same button + destination dedups
  // silently, a conflicting destination warns CCB-P1020 and the
  // real-code destination stands, real-only edges append, and the
  // sketch Home stays the entry screen.
  {
    char *proj = mk_real_proj(1);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *flow = join2(proj, "flow");
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *home = NULL;
    pv_edge *e = NULL;
    size_t count_new_note = 0;
    size_t i = 0;
    CHECK(proj != NULL);
    write_joined(flow, "Home.kt",
      "fun Home() {\n"
      "  Column {\n"
      "    Button(\"Open note\", "
      "onClick = { nav(\"Detail\", \"note-42\") })\n"
      "    Button(\"New note\", onClick = { nav(\"Form\") })\n"
      "  }\n"
      "}\n");
    write_joined(flow, "Detail.kt",
      "fun Detail() {\n"
      "  Column {\n"
      "    Text(\"Note detail\")\n"
      "  }\n"
      "}\n");
    write_joined(flow, "Form.kt",
      "fun Form() {\n"
      "  Column {\n"
      "    Text(\"New note\")\n"
      "  }\n"
      "}\n");
    write_joined(flow, "Settings.kt",
      "fun Settings() {\n"
      "  Column {\n"
      "    Text(\"Settings\")\n"
      "  }\n"
      "}\n");
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.content.Intent\n"
      "import android.os.Bundle\n"
      "import android.widget.Button\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "    findViewById<Button>(R.id.note1).setOnClickListener {\n"
      "      openDetail(\"note-1\")\n"
      "    }\n"
      "    findViewById<Button>(R.id.open_note).setOnClickListener {\n"
      "      startActivity(Intent(this, FormActivity::class.java))\n"
      "    }\n"
      "    findViewById<Button>(R.id.new_note).setOnClickListener {\n"
      "      startActivity(Intent(this, FormActivity::class.java))\n"
      "    }\n"
      "  }\n"
      "\n"
      "  private fun openDetail(noteId: String) {\n"
      "    val intent = Intent(this, DetailActivity::class.java)\n"
      "    startActivity(intent)\n"
      "  }\n"
      "}\n");
    write_joined(kd, "DetailActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class DetailActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_detail)\n"
      "  }\n"
      "}\n");
    write_joined(kd, "FormActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class FormActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_form)\n"
      "  }\n"
      "}\n");
    write_joined(kd, "SettingsActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class SettingsActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_settings)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <Button android:id=\"@+id/note1\" "
      "android:text=\"Note 1: Buy milk\" />\n"
      "  <Button android:id=\"@+id/open_note\" "
      "android:text=\"Open note\" />\n"
      "  <Button android:id=\"@+id/new_note\" "
      "android:text=\"New note\" />\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_detail.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_form.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_settings.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      ccb_str json;
      (void)pv_project_diagnose(p);
      CHECK(p->nscreens == 4);
      // The sketch Home stays the entry screen: the launcher
      // reorder only moves a screen to index 0 when no screen is
      // named "Home", so entry is proven by the JSON flag, not by
      // screens[0] (sketch parse order puts Detail first).
      ccb_str_init(&json);
      pv_project_json(&json, p);
      CHECK(json.buf != NULL &&
        strstr(json.buf, "\"name\":\"Home\",\"entry\":true") != NULL);
      ccb_str_free(&json);
      home = find_screen(p, "Home");
      CHECK(home != NULL);
      e = find_edge(home, "Open note");
      CHECK(e != NULL);
      if (e != NULL) {
        // Conflict: sketch says Detail("note-42"), real code says
        // Form with no args. Real code stands.
        CHECK_STR_EQ(e->target, "Form");
        CHECK(e->nargs == 0);
      }
      for (i = 0; i < home->nedges; i++) {
        if (home->edges[i].from != NULL &&
            strcmp(home->edges[i].from, "New note") == 0) {
          count_new_note++;
        }
      }
      CHECK(count_new_note == 1);
      e = find_edge(home, "New note");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Form");
      }
      e = find_edge(home, "Note 1: Buy milk");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
        CHECK(e->nargs == 1);
        CHECK(e->args != NULL && e->args[0] != NULL &&
          strcmp(e->args[0], "note-1") == 0);
      }
      CHECK(diag_count(p, "CCB-P1020") == 1);
      CHECK(diag_message_contains(p, "CCB-P1020", "Open note"));
      CHECK(diag_count(p, "CCB-P1025") == 0);
      CHECK(diag_count(p, "CCB-P1005") == 0);
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(flow);
    free(toml);
    free(proj);
  }

  // (3) Nested-lambda nav: no edge, CCB-P1025 naming the real file.
  // (A braced if body WOULD produce a branch-annotated edge; only
  // a nav inside a nested lambda/coroutine block is unattributed.)
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    const char *file = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.content.Intent\n"
      "import android.os.Bundle\n"
      "import android.widget.Button\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "    findViewById<Button>(R.id.nested).setOnClickListener {\n"
      "      runLater {\n"
      "        startActivity(Intent(this, DetailActivity::class.java))\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n");
    write_joined(kd, "DetailActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class DetailActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_detail)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <Button android:id=\"@+id/nested\" "
      "android:text=\"Nested\" />\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_detail.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      CHECK(find_edge(main_screen, "Nested") == NULL);
      CHECK(diag_count(p, "CCB-P1025") == 1);
      CHECK(diag_message_contains(p, "CCB-P1025", "nested block"));
      file = diag_file(p, "CCB-P1025", "nested block");
      CHECK(file != NULL);
      if (file != NULL) {
        size_t fl = strlen(file);
        CHECK(fl >= 15 &&
          strcmp(file + fl - 15, "MainActivity.kt") == 0);
      }
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(toml);
    free(proj);
  }

  // (4) Variable-receiver loop: no edge, and CCB-P1025 twice — once
  // for the unresolvable listener (with the REAL source line) and
  // once for the helper nav no button references.
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    const char *file = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n" // 1
      "\n" // 2
      "import android.app.Activity\n" // 3
      "import android.content.Intent\n" // 4
      "import android.os.Bundle\n" // 5
      "import android.widget.Button\n" // 6
      "\n" // 7
      "class MainActivity : Activity() {\n" // 8
      "  override fun onCreate(savedInstanceState: Bundle?) {\n" // 9
      "    super.onCreate(savedInstanceState)\n" // 10
      "    setContentView(R.layout.activity_main)\n" // 11
      "    val products = listOf(\"p1\", \"p2\")\n" // 12
      "    for ((buttonId, productId) in products) {\n" // 13
      "      findViewById<Button>(buttonId).setOnClickListener {\n" // 14
      "        openDetail(productId)\n" // 15
      "      }\n" // 16
      "    }\n" // 17
      "  }\n" // 18
      "\n" // 19
      "  private fun openDetail(productId: String) {\n" // 20
      "    val intent = Intent(this, DetailActivity::class.java)\n" // 21
      "    startActivity(intent)\n" // 22
      "  }\n" // 23
      "}\n"); // 24
    write_joined(kd, "DetailActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class DetailActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_detail)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <Button android:id=\"@+id/product1\" "
      "android:text=\"Product 1\" />\n"
      "  <Button android:id=\"@+id/product2\" "
      "android:text=\"Product 2\" />\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_detail.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      CHECK(find_edge(main_screen, "Product 1") == NULL);
      CHECK(find_edge(main_screen, "Product 2") == NULL);
      CHECK(diag_count(p, "CCB-P1025") == 2);
      CHECK(diag_message_contains(p, "CCB-P1025",
        "no literal view id"));
      CHECK(diag_message_contains(p, "CCB-P1025",
        "never referenced from a button"));
      CHECK(diag_line(p, "CCB-P1025", "no literal view id") == 14);
      file = diag_file(p, "CCB-P1025", "no literal view id");
      CHECK(file != NULL);
      if (file != NULL) {
        size_t fl = strlen(file);
        CHECK(fl >= 15 &&
          strcmp(file + fl - 15, "MainActivity.kt") == 0);
      }
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(toml);
    free(proj);
  }

  // (5) An Activity with no onCreate / setContentView yields no
  // screen, and no diagnostic for it.
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "  }\n"
      "}\n");
    write_joined(kd, "GhostActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "\n"
      "class GhostActivity : Activity()\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(p->nscreens == 1);
      CHECK(find_screen(p, "Main") != NULL);
      CHECK(find_screen(p, "Ghost") == NULL);
      CHECK(diag_count(p, "CCB-P1005") == 0);
      CHECK(diag_count(p, "CCB-P1025") == 0);
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(toml);
    free(proj);
  }

  // (6) navigate(R.id.x) resolves through the nav graph.
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *nd = nav_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    pv_edge *e = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "import android.widget.Button\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "    findViewById<Button>(R.id.go).setOnClickListener {\n"
      "      navigate(R.id.detail)\n"
      "    }\n"
      "  }\n"
      "}\n");
    write_joined(kd, "DetailActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class DetailActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_detail)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <Button android:id=\"@+id/go\" android:text=\"Go\" />\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_detail.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    write_joined(nd, "nav_graph.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<navigation xmlns:android="
      "\"http://schemas.android.com/apk/res/android\">\n"
      "  <activity android:id=\"@+id/home\" "
      "android:name=\"Home\" />\n"
      "  <activity android:id=\"@+id/detail\" "
      "android:name=\"Detail\" />\n"
      "</navigation>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(p->nscreens == 2);
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      e = find_edge(main_screen, "Go");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
      }
      CHECK(diag_count(p, "CCB-P1025") == 0);
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(nd);
    free(toml);
    free(proj);
  }
  // (7) No-sketch golden: p1-notes derived from real code only.
  // Paths inside diagnostics are normalized to PROJ so the golden
  // is independent of the scratch copy.
  {
    char *td = test_dir();
    char *rw = join2(td, "../examples/real-world");
    char scratch[64];
    ccb_test_tmpdir(scratch, sizeof scratch, "p2");
    char *src = NULL;
    char *copy = NULL;
    char *copy_toml = NULL;
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    pv_edge *e = NULL;
    struct stat st;
    CHECK(stat(rw, &st) == 0 && S_ISDIR(st.st_mode));
    src = join2(rw, "p1-notes-real");
    copy = join2(scratch, "p1-notes-real");
    copy_toml = join2(copy, "ccb.toml");
    CHECK(copy_tree(src, copy) == 0);
    p = load_project(copy_toml);
    CHECK(p != NULL);
    if (p != NULL) {
      ccb_str json;
      ccb_str tree;
      ccb_str rel;
      char *norm_json = NULL;
      char *norm_tree = NULL;
      (void)pv_project_diagnose(p);
      CHECK(p->nscreens == 4);
      CHECK(p->screens[0] != NULL &&
        strcmp(p->screens[0]->name, "Main") == 0);
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      e = find_edge(main_screen, "Note 1: Buy milk");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
        CHECK(e->nargs == 1);
        CHECK(e->args != NULL && e->args[0] != NULL &&
          strcmp(e->args[0], "note-1") == 0);
      }
      CHECK(diag_count(p, "CCB-P1005") == 0);
      CHECK(diag_count(p, "CCB-P1025") == 0);
      ccb_str_init(&json);
      pv_project_json(&json, p);
      ccb_str_init(&tree);
      pv_render_logic_tree(&tree, p);
      norm_json = replace_all(json.buf != NULL ? json.buf : "",
        copy, "PROJ");
      norm_tree = replace_all(tree.buf != NULL ? tree.buf : "",
        copy, "PROJ");
      ccb_str_init(&rel);
      ccb_str_append(&rel, "real-world/p1-notes-real/preview.json");
      check_golden_file("p1-notes-real", norm_json, rel.buf);
      ccb_str_free(&rel);
      ccb_str_init(&rel);
      ccb_str_append(&rel, "real-world/p1-notes-real/logic-tree.txt");
      check_golden_file("p1-notes-real", norm_tree, rel.buf);
      ccb_str_free(&rel);
      free(norm_json);
      free(norm_tree);
      ccb_str_free(&json);
      ccb_str_free(&tree);
      pv_project_free(p);
    }
    free(td);
    free(rw);
    free(src);
    free(copy);
    free(copy_toml);
    // scratch is the stack tmpl array mkdtemp modified in place:
    // never freed (same as the Chunk C golden test).
  }
  // (8) Chunk 2 wireframe: nested LinearLayouts map to column/row,
  // TextView/Button/EditText/ImageView map to their types, other
  // views become generic boxes — and buttons in a nested row still
  // produce edges.
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    pv_box *root = NULL;
    pv_box *col = NULL;
    pv_box *row = NULL;
    pv_edge *e = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.content.Intent\n"
      "import android.os.Bundle\n"
      "import android.widget.Button\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "    findViewById<Button>(R.id.btn_a).setOnClickListener {\n"
      "      startActivity(Intent(this, DetailActivity::class.java))\n"
      "    }\n"
      "    findViewById<Button>(R.id.btn_b).setOnClickListener {\n"
      "      startActivity(Intent(this, DetailActivity::class.java))\n"
      "    }\n"
      "  }\n"
      "}\n");
    write_joined(kd, "DetailActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class DetailActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_detail)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <TextView android:text=\"Hello\" />\n"
      "  <LinearLayout android:orientation=\"horizontal\">\n"
      "    <Button android:id=\"@+id/btn_a\" android:text=\"A\" />\n"
      "    <Button android:id=\"@+id/btn_b\" android:text=\"B\" />\n"
      "  </LinearLayout>\n"
      "  <EditText android:id=\"@+id/name\" android:text=\"Name\" />\n"
      "  <ImageView android:id=\"@+id/pic\" />\n"
      "  <RecyclerView android:id=\"@+id/list\" />\n"
      "</LinearLayout>\n");
    write_joined(ld, "activity_detail.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <TextView android:text=\"Detail\" />\n"
      "</LinearLayout>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      root = main_screen != NULL ? main_screen->root : NULL;
      CHECK(root != NULL);
      CHECK(root != NULL && root->type != NULL &&
        strcmp(root->type, "screen") == 0);
      CHECK(root != NULL && root->nchildren == 1);
      col = box_child(root, 0);
      CHECK(col != NULL && col->type != NULL &&
        strcmp(col->type, "column") == 0);
      CHECK(col != NULL && col->nchildren == 5);
      CHECK(box_child(col, 0) != NULL &&
        strcmp(box_child(col, 0)->type, "text") == 0 &&
        box_child(col, 0)->label != NULL &&
        strcmp(box_child(col, 0)->label, "Hello") == 0);
      row = box_child(col, 1);
      CHECK(row != NULL && row->type != NULL &&
        strcmp(row->type, "row") == 0);
      CHECK(row != NULL && row->nchildren == 2);
      CHECK(box_child(row, 0) != NULL &&
        strcmp(box_child(row, 0)->type, "button") == 0 &&
        box_child(row, 0)->label != NULL &&
        strcmp(box_child(row, 0)->label, "A") == 0);
      CHECK(box_child(row, 1) != NULL &&
        strcmp(box_child(row, 1)->type, "button") == 0 &&
        box_child(row, 1)->label != NULL &&
        strcmp(box_child(row, 1)->label, "B") == 0);
      CHECK(box_child(col, 2) != NULL &&
        strcmp(box_child(col, 2)->type, "textfield") == 0 &&
        box_child(col, 2)->label != NULL &&
        strcmp(box_child(col, 2)->label, "Name") == 0);
      CHECK(box_child(col, 3) != NULL &&
        strcmp(box_child(col, 3)->type, "image") == 0 &&
        box_child(col, 3)->label == NULL);
      CHECK(box_child(col, 4) != NULL &&
        strcmp(box_child(col, 4)->type, "box") == 0);
      e = find_edge(main_screen, "A");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
      }
      e = find_edge(main_screen, "B");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
      }
      CHECK(diag_count(p, "CCB-P1025") == 0);
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(toml);
    free(proj);
  }

  // (9) Chunk 2 wireframe: an @string label resolves through the
  // normal resource pass, like any component label.
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *vd = values_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    pv_box *col = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "  <TextView android:text=\"@string/hello\" />\n"
      "</LinearLayout>\n");
    write_joined(vd, "strings.xml",
      "<resources>\n"
      "  <string name=\"hello\">Hello world</string>\n"
      "</resources>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      col = main_screen != NULL ?
        box_child(main_screen->root, 0) : NULL;
      CHECK(col != NULL && col->type != NULL &&
        strcmp(col->type, "column") == 0);
      CHECK(box_child(col, 0) != NULL &&
        strcmp(box_child(col, 0)->type, "text") == 0 &&
        box_child(col, 0)->label != NULL &&
        strcmp(box_child(col, 0)->label, "Hello world") == 0);
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(vd);
    free(toml);
    free(proj);
  }

  // (10) Chunk 2 wireframe: a ConstraintLayout root is a generic
  // box (no solving), with its children still mapped.
  {
    char *proj = mk_real_proj(0);
    char *kd = kt_dir(proj);
    char *ld = layout_dir(proj);
    char *toml = join2(proj, "ccb.toml");
    pv_project *p = NULL;
    pv_screen *main_screen = NULL;
    pv_box *box = NULL;
    pv_edge *e = NULL;
    CHECK(proj != NULL);
    write_joined(kd, "MainActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.content.Intent\n"
      "import android.os.Bundle\n"
      "import android.widget.Button\n"
      "\n"
      "class MainActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_main)\n"
      "    findViewById<Button>(R.id.go).setOnClickListener {\n"
      "      startActivity(Intent(this, DetailActivity::class.java))\n"
      "    }\n"
      "  }\n"
      "}\n");
    write_joined(kd, "DetailActivity.kt",
      "package com.example.app\n"
      "\n"
      "import android.app.Activity\n"
      "import android.os.Bundle\n"
      "\n"
      "class DetailActivity : Activity() {\n"
      "  override fun onCreate(savedInstanceState: Bundle?) {\n"
      "    super.onCreate(savedInstanceState)\n"
      "    setContentView(R.layout.activity_detail)\n"
      "  }\n"
      "}\n");
    write_joined(ld, "activity_main.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<androidx.constraintlayout.widget.ConstraintLayout "
      "xmlns:android="
      "\"http://schemas.android.com/apk/res/android\">\n"
      "  <TextView android:text=\"Title\" />\n"
      "  <Button android:id=\"@+id/go\" android:text=\"Go\" />\n"
      "</androidx.constraintlayout.widget.ConstraintLayout>\n");
    write_joined(ld, "activity_detail.xml",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<LinearLayout xmlns:android="
      "\"http://schemas.android.com/apk/res/android\"\n"
      "  android:orientation=\"vertical\">\n"
      "</LinearLayout>\n");
    p = load_project(toml);
    CHECK(p != NULL);
    if (p != NULL) {
      main_screen = find_screen(p, "Main");
      CHECK(main_screen != NULL);
      box = main_screen != NULL ?
        box_child(main_screen->root, 0) : NULL;
      CHECK(box != NULL && box->type != NULL &&
        strcmp(box->type, "box") == 0);
      CHECK(box != NULL && box->nchildren == 2);
      CHECK(box_child(box, 0) != NULL &&
        strcmp(box_child(box, 0)->type, "text") == 0 &&
        box_child(box, 0)->label != NULL &&
        strcmp(box_child(box, 0)->label, "Title") == 0);
      CHECK(box_child(box, 1) != NULL &&
        strcmp(box_child(box, 1)->type, "button") == 0 &&
        box_child(box, 1)->label != NULL &&
        strcmp(box_child(box, 1)->label, "Go") == 0);
      e = find_edge(main_screen, "Go");
      CHECK(e != NULL);
      if (e != NULL) {
        CHECK_STR_EQ(e->target, "Detail");
      }
      pv_project_free(p);
    }
    free(kd);
    free(ld);
    free(toml);
    free(proj);
  }
  TEST_MAIN_END();
}
