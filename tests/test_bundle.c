// Tests for ccb_bundle: BundleConfig.pb hand encoding (encode -> decode
// round-trip, exact bytes for the known version, malformed input).
#include "test.h"
#include "ccb_bundle.h"

int main(void) {
  // Exact wire bytes for version "1.0.0": outer tag 0x0A, len 7, inner
  // tag 0x12 (field 2), len 5, "1.0.0".
  {
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(ccb_bundle_config_encode("1.0.0", &out, &out_len) == CCB_OK);
    static const uint8_t expect[] = { 0x0A, 0x07, 0x12, 0x05, '1', '.', '0', '.', '0' };
    CHECK(out_len == sizeof(expect));
    CHECK(memcmp(out, expect, sizeof(expect)) == 0);
    free(out);
  }
  // Round-trip for several versions, including an empty one and a long
  // one (multi-byte varint lengths are exercised past 127 bytes).
  {
    const char *versions[] = { "", "2.4.21", "bundletool-1.17.2" };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
      uint8_t *out = NULL;
      size_t out_len = 0;
      CHECK(ccb_bundle_config_encode(versions[i], &out, &out_len) == CCB_OK);
      char *back = NULL;
      CHECK(ccb_bundle_config_decode_version(out, out_len, &back) == CCB_OK);
      CHECK_STR_EQ(back, versions[i]);
      free(out);
      free(back);
    }
    char long_ver[200];
    memset(long_ver, 'v', sizeof(long_ver) - 1);
    long_ver[sizeof(long_ver) - 1] = '\0';
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(ccb_bundle_config_encode(long_ver, &out, &out_len) == CCB_OK);
    char *back = NULL;
    CHECK(ccb_bundle_config_decode_version(out, out_len, &back) == CCB_OK);
    CHECK_STR_EQ(back, long_ver);
    free(out);
    free(back);
  }
  // Malformed input must fail, never crash or accept.
  {
    uint8_t *good = NULL;
    size_t good_len = 0;
    CHECK(ccb_bundle_config_encode("1.0.0", &good, &good_len) == CCB_OK);
    char *back = NULL;
    // Truncated (missing last byte).
    CHECK(ccb_bundle_config_decode_version(good, good_len - 1, &back) == CCB_ERR_IO);
    // Trailing garbage.
    uint8_t *trailing = malloc(good_len + 1);
    memcpy(trailing, good, good_len);
    trailing[good_len] = 0x00;
    CHECK(ccb_bundle_config_decode_version(trailing, good_len + 1, &back) == CCB_ERR_IO);
    free(trailing);
    // Wrong outer tag.
    uint8_t bad_tag[9];
    memcpy(bad_tag, good, good_len);
    bad_tag[0] = 0x0B;
    CHECK(ccb_bundle_config_decode_version(bad_tag, good_len, &back) == CCB_ERR_IO);
    // Length overrun: outer length claims more than present.
    uint8_t bad_len[9];
    memcpy(bad_len, good, good_len);
    bad_len[1] = 0x40;
    CHECK(ccb_bundle_config_decode_version(bad_len, good_len, &back) == CCB_ERR_IO);
    // Empty input.
    CHECK(ccb_bundle_config_decode_version(good, 0, &back) == CCB_ERR_IO);
    // NULL guards.
    CHECK(ccb_bundle_config_encode(NULL, &good, &good_len) == CCB_ERR_IO);
    CHECK(ccb_bundle_config_decode_version(NULL, good_len, &back) == CCB_ERR_IO);
    free(good);
  }
  TEST_MAIN_END();
}
