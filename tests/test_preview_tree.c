// Tests for preview tree-live mode (Chunk 4): the pv_tree state
// machine only, driven by scripted keys and canned publishTree
// payloads with no TTY and no child process. The terminal layer
// (pv_tree_run) is not exercised here: it refuses non-TTY stdio,
// so it cannot run inside this harness by design — the phone test
// covers it with a scripted pty session, the way the walk test
// covers walk mode.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_json.h"
#include "ccb_preview_tree.h"
#include "ccb_str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- canned publishTree params payloads ----------

static const char *kTree1 =
  "{\"event\":\"preview\",\"version\":2,\"scenario\":\"loaded\","
  "\"trace\":{\"steps\":2},\"screens\":["
  "{\"name\":\"Home\",\"entry\":true,\"reachable\":true,\"edges\":["
  "{\"from\":\"Open note\",\"target\":\"Detail\",\"unknown\":false,"
  "\"args\":[\"note-42\"],\"branch\":null},"
  "{\"from\":\"New\",\"target\":\"Form\",\"unknown\":false,\"args\":[],"
  "\"branch\":null}],\"branches\":[]},"
  "{\"name\":\"Detail\",\"entry\":false,\"reachable\":true,\"edges\":["
  "{\"from\":\"Back\",\"target\":\"Home\",\"unknown\":false,"
  "\"args\":[\"42\"],\"args_provenance\":[\"scenario\"],"
  "\"branch\":null,\"taken\":true},"
  "{\"from\":\"More\",\"target\":null,\"unknown\":true,\"args\":[],"
  "\"branch\":null}],"
  "\"branches\":[{\"label\":\"when state\",\"entries\":[\"is Loading\","
  "\"is Loaded\",\"else\"],\"picked\":{\"entry\":\"is Loaded\","
  "\"provenance\":\"scenario\"}}]},"
  "{\"name\":\"Form\",\"entry\":false,\"reachable\":false,\"edges\":["
  "{\"from\":\"Save\",\"target\":\"Home\",\"unknown\":false,\"args\":[],"
  "\"branch\":\"if ready: then\"}],\"branches\":[]}],"
  "\"diagnostics\":["
  "{\"code\":\"CCB-P1019\",\"phase\":\"flow\",\"message\":\"handler found, "
  "button not bound: openButton\",\"file\":\"flow/Home.kt\","
  "\"line\":12},"
  "{\"code\":\"CCB-P1006\",\"phase\":\"\",\"message\":\"unreachable screen: "
  "Form\",\"file\":null,\"line\":0}]}";

// Refresh payload: a new Settings screen appears, and Home's "New"
// edge now targets it. Detail is unchanged.
static const char *kTree2 =
  "{\"event\":\"preview\",\"version\":2,\"screens\":["
  "{\"name\":\"Home\",\"entry\":true,\"reachable\":true,\"edges\":["
  "{\"from\":\"Open note\",\"target\":\"Detail\",\"unknown\":false,"
  "\"args\":[\"note-42\"],\"branch\":null},"
  "{\"from\":\"New\",\"target\":\"Settings\",\"unknown\":false,\"args\":[],"
  "\"branch\":null}],\"branches\":[]},"
  "{\"name\":\"Detail\",\"entry\":false,\"reachable\":true,\"edges\":["
  "{\"from\":\"Back\",\"target\":\"Home\",\"unknown\":false,"
  "\"args\":[\"42\"],\"branch\":null}],\"branches\":[]},"
  "{\"name\":\"Form\",\"entry\":false,\"reachable\":false,\"edges\":[],"
  "\"branches\":[]},"
  "{\"name\":\"Settings\",\"entry\":false,\"reachable\":true,\"edges\":["
  "{\"from\":\"Done\",\"target\":\"Home\",\"unknown\":false,\"args\":[],"
  "\"branch\":null}],\"branches\":[]}],\"diagnostics\":[]}";

static const char *kEmptyTree =
  "{\"event\":\"preview\",\"version\":2,\"screens\":[],\"diagnostics\":[]}";

// Build one framed JSON-RPC message (malloc'd, caller frees).
static char *frame_msg(const char *body) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_appendf(&out, "Content-Length: %zu\r\n\r\n",
    strlen(body));
  ccb_str_append(&out, body);
  return out.buf;
}

// Wrap a tree payload in a ccb/publishTree notification body
// (malloc'd, caller frees), the way the server sends it.
static char *publish_msg(const char *tree_json) {
  ccb_str out;
  ccb_str_init(&out);
  ccb_str_append(&out, "{\"jsonrpc\":\"2.0\",\"method\":"
    "\"ccb/publishTree\",\"params\":");
  ccb_str_append(&out, tree_json);
  ccb_str_append(&out, "}");
  return out.buf;
}

// Render the tree to a malloc'd string (caller frees).
static char *render_tree(const pv_tree *t) {
  ccb_str out;
  ccb_str_init(&out);
  pv_tree_render(&out, t);
  if (out.buf == NULL) {
    return ccb_xstrdup("");
  }
  return out.buf;
}

static int contains(const char *hay, const char *needle) {
  return hay != NULL && needle != NULL &&
    strstr(hay, needle) != NULL;
}

int main(void) {
  pv_tree t;
  char *text = NULL;
  pv_tree_init(&t);

  // Waiting view before the first publish.
  text = render_tree(&t);
  CHECK(contains(text, "waiting for logic tree"));
  CHECK(contains(text, "q: quit"));
  free(text);

  // First publish: screens, scenario/trace header, selection.
  CHECK(pv_tree_apply_publish(&t, kTree1) == 0);
  CHECK(t.have_tree == 1);
  CHECK(t.nscreens == 3);
  CHECK(t.sel == 0);
  CHECK_STR_EQ(t.scenario, "loaded");
  CHECK(t.trace_steps == 2);
  CHECK(pv_tree_selected(&t) != NULL);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Home");
  text = render_tree(&t);
  CHECK(contains(text, "scenario: loaded | trace: 2 steps"));
  CHECK(contains(text, "3 screens"));
  CHECK(contains(text, "> Home [entry]"));
  CHECK(contains(text, "  Detail\n"));
  CHECK(contains(text, "  Form (unreachable)"));
  // Home's detail pane: outgoing + incoming + diagnostics.
  CHECK(contains(text, "Home [entry]:"));
  CHECK(contains(text, "-> Detail(\"note-42\") via \"Open note\""));
  CHECK(contains(text, "-> Form via \"New\""));
  CHECK(contains(text, "<- Detail via \"Back\"(\"42\" [scenario]) [trace]"));
  CHECK(contains(text, "<- Form via \"Save\" [if ready: then]"));
  CHECK(contains(text, "(no branches on this screen)"));
  CHECK(contains(text, "CCB-P1019 handler found, button not bound: "
    "openButton (flow/Home.kt:12)"));
  CHECK(contains(text, "CCB-P1006 unreachable screen: Form"));
  free(text);

  // Scripted keys: down into Detail, clamps at the ends.
  pv_tree_apply_key(&t, PV_TREE_KEY_DOWN);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Detail");
  text = render_tree(&t);
  CHECK(contains(text, "> Detail\n"));
  CHECK(contains(text, "-> Home(\"42\" [scenario]) via \"Back\" [trace]"));
  CHECK(contains(text, "-> unknown via \"More\""));
  CHECK(contains(text, "<- Home via \"Open note\"(\"note-42\")"));
  CHECK(contains(text, "when state: is Loading, is Loaded, else "
    "(picked: is Loaded [scenario])"));
  free(text);
  pv_tree_apply_key(&t, PV_TREE_KEY_DOWN);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Form");
  pv_tree_apply_key(&t, PV_TREE_KEY_DOWN);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Form"); // clamped
  pv_tree_apply_key(&t, PV_TREE_KEY_UP);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Detail");
  pv_tree_apply_key(&t, PV_TREE_KEY_UP);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Home");
  pv_tree_apply_key(&t, PV_TREE_KEY_UP);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Home"); // clamped

  // Select by name; unknown names leave the selection alone.
  CHECK(pv_tree_select(&t, "Form") == 1);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Form");
  CHECK(pv_tree_select(&t, "Nope") == 0);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Form");

  // Refresh: selection follows the NAME (Detail survives the
  // refresh even though a screen was inserted after it), the new
  // screen appears, and the changed edge is visible.
  CHECK(pv_tree_select(&t, "Detail") == 1);
  CHECK(pv_tree_apply_publish(&t, kTree2) == 0);
  CHECK(t.nscreens == 4);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Detail");
  text = render_tree(&t);
  CHECK(contains(text, "  Settings\n"));
  CHECK(contains(text, "4 screens"));
  free(text);
  CHECK(pv_tree_select(&t, "Home") == 1);
  text = render_tree(&t);
  CHECK(contains(text, "-> Settings via \"New\""));
  free(text);

  // Selection falls back to the clamped index when the selected
  // screen disappears: select Settings (index 3), refresh with a
  // tree that drops it, and the selection lands on Form (index 2).
  CHECK(pv_tree_select(&t, "Settings") == 1);
  CHECK(pv_tree_apply_publish(&t, kTree1) == 0);
  CHECK(t.nscreens == 3);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Form");
  pv_tree_apply_key(&t, PV_TREE_KEY_QUIT);
  CHECK(t.quit == 1);
  pv_tree_free(&t);

  // Malformed publishes keep the old tree.
  pv_tree_init(&t);
  CHECK(pv_tree_apply_publish(&t, kTree1) == 0);
  CHECK(pv_tree_apply_publish(&t, "not json") == -1);
  CHECK(pv_tree_apply_publish(&t, "{\"screens\":\"nope\"}") == -1);
  CHECK(pv_tree_apply_publish(&t, "{\"diagnostics\":[]}") == -1);
  CHECK(t.nscreens == 3);
  CHECK_STR_EQ(pv_tree_selected(&t)->name, "Home");
  pv_tree_free(&t);

  // Empty tree renders, and selection is empty.
  pv_tree_init(&t);
  CHECK(pv_tree_apply_publish(&t, kEmptyTree) == 0);
  CHECK(t.have_tree == 1);
  CHECK(t.nscreens == 0);
  CHECK(pv_tree_selected(&t) == NULL);
  pv_tree_apply_key(&t, PV_TREE_KEY_DOWN); // no-op, no crash
  CHECK(pv_tree_selected(&t) == NULL);
  text = render_tree(&t);
  CHECK(contains(text, "no screens in this project"));
  free(text);
  pv_tree_free(&t);

  // Feed framing: split header/body, two frames in one chunk,
  // responses ignored, garbage headers skipped, bad JSON skipped.
  pv_tree_init(&t);
  {
    char *pub1 = publish_msg(kTree1);
    char *pub2 = publish_msg(kTree2);
    char *full = frame_msg(pub1);
    size_t full_len = strlen(full);
    size_t split = full_len / 2;
    char *resp = NULL;
    char *garbage = NULL;
    char *bad = NULL;
    char *both = NULL;
    ccb_str tmp;
    CHECK(pv_tree_feed(&t, full, split) == 0);
    CHECK(t.have_tree == 0);
    CHECK(pv_tree_feed(&t, full + split, full_len - split) == 1);
    CHECK(t.have_tree == 1);
    CHECK(t.nscreens == 3);
    free(full);
    // A response (id, no method) changes nothing.
    resp = frame_msg("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    CHECK(pv_tree_feed(&t, resp, strlen(resp)) == 0);
    CHECK(t.nscreens == 3);
    free(resp);
    // Garbage header line, then a valid frame: the frame applies.
    ccb_str_init(&tmp);
    ccb_str_append(&tmp, "Bogus: nope\r\n\r\n");
    garbage = frame_msg(pub2);
    ccb_str_append(&tmp, garbage);
    CHECK(pv_tree_feed(&t, tmp.buf, strlen(tmp.buf)) == 1);
    CHECK(t.nscreens == 4);
    ccb_str_free(&tmp);
    free(garbage);
    // A framed but unparseable body is skipped, tree kept.
    bad = frame_msg("{not json");
    CHECK(pv_tree_feed(&t, bad, strlen(bad)) == 0);
    CHECK(t.nscreens == 4);
    free(bad);
    // Two valid frames in one chunk apply in order (last wins).
    ccb_str_init(&tmp);
    both = frame_msg(pub1);
    ccb_str_append(&tmp, both);
    free(both);
    both = frame_msg(pub2);
    ccb_str_append(&tmp, both);
    free(both);
    CHECK(pv_tree_feed(&t, tmp.buf, strlen(tmp.buf)) == 2);
    CHECK(t.nscreens == 4);
    CHECK_STR_EQ(pv_tree_selected(&t)->name, "Home");
    ccb_str_free(&tmp);
    free(pub1);
    free(pub2);
  }
  pv_tree_free(&t);

  TEST_MAIN_END();
}
