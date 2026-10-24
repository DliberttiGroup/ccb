// Tests for ccb_json parsing and escaping.
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_json.h"
#include "ccb_str.h"

int main(void) {
  // Escaping.
  {
    ccb_str out;
    ccb_str_init(&out);
    ccb_json_escape(&out, "a\"b\\c\nd");
    CHECK_STR_EQ(out.buf, "\"a\\\"b\\\\c\\nd\"");
    ccb_str_free(&out);
  }
  // Object parsing.
  {
    char *err = NULL;
    ccb_json_value *v = ccb_json_parse(ccb_slice_from(
      "{\"name\":\"ccb\",\"version\":3,\"ok\":true,\"none\":null,\"list\":[1,2.5,\"x\"]}"), &err);
    CHECK(v != NULL);
    CHECK(err == NULL);
    const ccb_json_value *name = ccb_json_obj_get(v, "name");
    CHECK(name != NULL && name->type == CCB_JSON_STR);
    CHECK_STR_EQ(name->str, "ccb");
    const ccb_json_value *ver = ccb_json_obj_get(v, "version");
    CHECK(ver != NULL && ver->num == 3.0);
    const ccb_json_value *ok = ccb_json_obj_get(v, "ok");
    CHECK(ok != NULL && ok->boolean == true);
    const ccb_json_value *none = ccb_json_obj_get(v, "none");
    CHECK(none != NULL && none->type == CCB_JSON_NULL);
    const ccb_json_value *list = ccb_json_obj_get(v, "list");
    CHECK(list != NULL && list->count == 3);
    CHECK(list->items[2]->type == CCB_JSON_STR);
    CHECK_STR_EQ(list->items[2]->str, "x");
    CHECK(ccb_json_obj_get(v, "missing") == NULL);
    ccb_json_free(v);
  }
  // String escapes, including a surrogate pair (U+1F600).
  {
    char *err = NULL;
    ccb_json_value *v = ccb_json_parse(ccb_slice_from("\"a\\n\\u0041\\ud83d\\ude00\""), &err);
    CHECK(v != NULL);
    CHECK(v->type == CCB_JSON_STR);
    CHECK_STR_EQ(v->str, "a\nA\xf0\x9f\x98\x80");
    ccb_json_free(v);
  }
  // Errors.
  {
    char *err = NULL;
    ccb_json_value *v = ccb_json_parse(ccb_slice_from("{\"a\":}"), &err);
    CHECK(v == NULL);
    CHECK(err != NULL);
    free(err);
    err = NULL;
    v = ccb_json_parse(ccb_slice_from("[1,2"), &err);
    CHECK(v == NULL);
    CHECK(err != NULL);
    free(err);
    err = NULL;
    v = ccb_json_parse(ccb_slice_from("{\"a\":1} trailing"), &err);
    CHECK(v == NULL);
    CHECK(err != NULL);
    free(err);
  }
  TEST_MAIN_END();
}
