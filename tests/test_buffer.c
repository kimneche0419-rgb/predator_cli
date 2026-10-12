#include "../src/core/buffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_basic_ascii(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 64) == 0);
    assert(lvc_buf_insert(&b, 0, (const uint8_t *)"hello world", 11) == 11);
    assert(lvc_buf_insert(&b, 5, (const uint8_t *)",", 1) == 1);
    uint8_t out[16] = {0};
    lvc_buf_read(&b, 0, out, 12);
    assert(strcmp((char *)out, "hello, world") == 0);
    assert(lvc_buf_delete(&b, 0, 7) == 7); /* remove "hello, " */
    lvc_buf_read(&b, 0, out, 5);
    out[5] = 0;
    assert(strcmp((char *)out, "world") == 0);
    assert(lvc_buf_len(&b) == 5);
    lvc_buf_free(&b);
}

static void test_utf8(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 64) == 0);
    const uint8_t *ko = (const uint8_t *)"한글abc";
    assert(lvc_buf_insert(&b, 0, ko, strlen((const char *)ko)) == 9);
    uint8_t out[9];
    lvc_buf_read(&b, 0, out, 9);
    assert(memcmp(out, ko, 9) == 0);
    assert(lvc_buf_len(&b) == 9);
    lvc_buf_free(&b);
}

static void test_boundaries_and_invalid(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 64) == 0);
    const uint8_t *s = (const uint8_t *)"가나다"; /* 3 bytes each */
    assert(lvc_buf_insert(&b, 0, s, 9) == 9);
    /* insert inside a multi-byte char must fail */
    assert(lvc_buf_insert(&b, 1, (const uint8_t *)"x", 1) == -1);
    /* invalid UTF-8 insert must fail */
    uint8_t bad[] = {0xFF, 0xFE};
    assert(lvc_buf_insert(&b, 9, bad, 2) == -1);
    /* delete mid-char must fail */
    assert(lvc_buf_delete(&b, 1, 2) == -1);
    /* delete whole first char ok */
    assert(lvc_buf_delete(&b, 0, 3) == 3);
    uint8_t out[8];
    lvc_buf_read(&b, 0, out, 6);
    assert(memcmp(out, s + 3, 6) == 0);
    lvc_buf_free(&b);
}

static void test_lines_and_growth(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 16) == 0); /* small cap -> force growth */
    for (int i = 0; i < 100; i++)
        assert(lvc_buf_insert(&b, lvc_buf_len(&b), (const uint8_t *)"line\n", 5) == 5);
    assert(lvc_buf_line_count(&b) == 101); /* 100 newlines -> trailing empty line */
    assert(lvc_buf_len(&b) == 500);
    lvc_buf_free(&b);
}

int main(void) {
    test_basic_ascii();
    test_utf8();
    test_boundaries_and_invalid();
    test_lines_and_growth();
    printf("all buffer tests passed\n");
    return 0;
}
