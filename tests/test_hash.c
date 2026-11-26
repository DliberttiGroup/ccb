// Tests for ccb_hash (SHA-256 known vectors).
#include "test.h"
#include "ccb_fs.h"
#include "ccb_hash.h"

#define B(rel) ccb_test_path(dir, rel)

int main(void) {
  char dir[256];
  ccb_test_tmpdir(dir, sizeof dir, "hash");
  char hex[65];
  ccb_sha256_hex("", 0, hex);
  CHECK_STR_EQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  ccb_sha256_hex("abc", 3, hex);
  CHECK_STR_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  ccb_sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
  CHECK_STR_EQ(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // One million 'a' characters.
  {
    char *buf = malloc(1000000);
    memset(buf, 'a', 1000000);
    ccb_sha256_hex(buf, 1000000, hex);
    CHECK_STR_EQ(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(buf);
  }
  // File hashing.
  {
    FILE *f = fopen(B("hash.txt"), "wb");
    fputs("hello", f);
    fclose(f);
    CHECK(ccb_sha256_file(B("hash.txt"), hex) == CCB_OK);
    CHECK_STR_EQ(hex, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    remove(B("hash.txt"));
    CHECK(ccb_sha256_file(B("missing.txt"), hex) == CCB_ERR_IO);
  }
  ccb_remove_recursive(dir);
  TEST_MAIN_END();
}
