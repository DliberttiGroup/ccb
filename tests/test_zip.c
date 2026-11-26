// Tests for ccb_zip (CRC32, inflate, writer/reader round-trip).
#include "test.h"
#include "ccb_alloc.h"
#include "ccb_fs.h"
#include "ccb_zip.h"

#define B(rel) ccb_test_path(dir, rel)

int main(void) {
  char dir[256];
  ccb_test_tmpdir(dir, sizeof dir, "zip");
  // CRC32 vectors.
  CHECK(ccb_zip_crc32("", 0) == 0);
  CHECK(ccb_zip_crc32("123456789", 9) == 0xCBF43926u);
  CHECK(ccb_zip_crc32("hello", 5) == 0x3610A686u);

  // Inflate: stored block for "hello".
  {
    static const uint8_t comp[] = { 0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o' };
    uint8_t out[5];
    CHECK(ccb_inflate(comp, sizeof(comp), out, 5) == CCB_OK);
    CHECK(memcmp(out, "hello", 5) == 0);
  }
  // Inflate: empty fixed-Huffman block.
  {
    static const uint8_t comp[] = { 0x03, 0x00 };
    uint8_t out[1];
    CHECK(ccb_inflate(comp, sizeof(comp), out, 0) == CCB_OK);
  }
  // Inflate: fixed-Huffman "hello world, hello zip world! " x4 (from zlib).
  {
    static const uint8_t comp[] = {
      0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xd1, 0x51,
      0xc8, 0x00, 0x73, 0xaa, 0x32, 0x0b, 0x20, 0x02, 0x8a, 0x50, 0x01, 0x5a, 0xc8,
      0x02, 0x00
    };
    char expect[121];
    expect[0] = '\0';
    for (int i = 0; i < 4; i++) {
      strcat(expect, "hello world, hello zip world! ");
    }
    uint8_t out[120];
    CHECK(ccb_inflate(comp, sizeof(comp), out, 120) == CCB_OK);
    CHECK(memcmp(out, expect, 120) == 0);
    // Wrong output size must fail.
    CHECK(ccb_inflate(comp, sizeof(comp), out, 119) == CCB_ERR_IO);
  }
  // Inflate: corrupt input must fail.
  {
    static const uint8_t comp[] = { 0xcb, 0x48, 0xcd, 0xc9, 0x00, 0x00 };
    uint8_t out[120];
    CHECK(ccb_inflate(comp, sizeof(comp), out, 120) == CCB_ERR_IO);
  }
  // Inflate: invalid block type must fail.
  {
    static const uint8_t comp[] = { 0x07, 0x00 };
    uint8_t out[4];
    CHECK(ccb_inflate(comp, sizeof(comp), out, 4) == CCB_ERR_IO);
  }

  // Writer/reader round-trip.
  {
    ccb_remove_recursive(dir);
    CHECK(ccb_mkdir_p(dir) == CCB_OK);
    CHECK(ccb_write_file(B("a.txt"), "alpha", 5) == CCB_OK);
    ccb_zip_writer w;
    CHECK(ccb_zip_writer_init(&w, B("out.zip")) == CCB_OK);
    CHECK(ccb_zip_writer_add_file(&w, "res/a.txt", B("a.txt")) == CCB_OK);
    CHECK(ccb_zip_writer_add_data(&w, "empty.bin", "", 0) == CCB_OK);
    static const char bin[4] = { 0x00, 0x01, 0x02, (char)0xFF };
    CHECK(ccb_zip_writer_add_data(&w, "bin.dat", bin, 4) == CCB_OK);
    // Duplicate names and unsafe names are rejected.
    CHECK(ccb_zip_writer_add_data(&w, "bin.dat", bin, 4) == CCB_ERR_IO);
    CHECK(ccb_zip_writer_add_data(&w, "../escape.txt", "x", 1) == CCB_ERR_IO);
    CHECK(ccb_zip_writer_add_data(&w, "/absolute.txt", "x", 1) == CCB_ERR_IO);
    CHECK(ccb_zip_writer_finish(&w) == CCB_OK);

    ccb_zip_archive *a = NULL;
    CHECK(ccb_zip_open(B("out.zip"), &a) == CCB_OK);
    CHECK(a->count == 3);
    const ccb_zip_entry *e = ccb_zip_find(a, "res/a.txt");
    CHECK(e != NULL && e->method == 0 && e->size == 5);
    void *data = NULL;
    size_t len = 0;
    CHECK(ccb_zip_read_entry(a, e, &data, &len) == CCB_OK);
    CHECK(len == 5 && memcmp(data, "alpha", 5) == 0);
    free(data);
    e = ccb_zip_find(a, "bin.dat");
    CHECK(e != NULL);
    CHECK(ccb_zip_read_entry(a, e, &data, &len) == CCB_OK);
    CHECK(len == 4 && memcmp(data, bin, 4) == 0);
    free(data);
    CHECK(ccb_zip_find(a, "missing.txt") == NULL);
    ccb_zip_close(a);
    ccb_remove_recursive(dir);
  }
  ccb_remove_recursive(dir);
  TEST_MAIN_END();
}
