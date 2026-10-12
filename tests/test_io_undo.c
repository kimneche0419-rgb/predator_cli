#include "../src/core/io.h"
#include "../src/core/undo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_io_roundtrip(void) {
    const char *path = "/tmp/lvc_io_test.txt";
    lvc_buf b;
    assert(lvc_buf_init(&b, 64) == 0);
    const char *doc = "first\n둘째\n";
    size_t doc_len = strlen(doc);
    assert(lvc_buf_insert(&b, 0, (const uint8_t *)doc, doc_len) == (ptrdiff_t)doc_len);
    assert(lvc_io_save(&b, path) == 0);

    lvc_buf b2;
    assert(lvc_buf_init(&b2, 64) == 0);
    assert(lvc_buf_insert(&b2, 0, (const uint8_t *)"junk", 4) == 4);
    assert(lvc_io_load(&b2, path) == 0);
    assert(lvc_buf_len(&b2) == doc_len);
    uint8_t out[32] = {0};
    lvc_buf_read(&b2, 0, out, doc_len);
    assert(memcmp(out, doc, doc_len) == 0);
    remove(path);
    lvc_buf_free(&b);
    lvc_buf_free(&b2);
}

static void test_io_missing(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 64) == 0);
    assert(lvc_io_load(&b, "/nonexistent/file/xyz") == -1);
    lvc_buf_free(&b);
}

static void test_undo_insert_delete(void) {
    lvc_buf b;
    lvc_undo u;
    assert(lvc_buf_init(&b, 64) == 0);
    assert(lvc_undo_init(&u, 100) == 0);

    const char *msg = "hello";
    assert(lvc_buf_insert(&b, 0, (const uint8_t *)msg, 5) == 5);
    assert(lvc_undo_push(&u, LVC_OP_INSERT, 0, (const uint8_t *)msg, 5, u.next_group) == 0);

    assert(lvc_undo_delete(&u, &b, 0, 3, ++u.next_group) == 3); /* remove "hel" */

    /* undo delete -> "hello" */
    assert(lvc_undo_undo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 5);
    /* undo insert -> "" */
    assert(lvc_undo_undo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 0);
    assert(lvc_undo_undo(&u, &b) == 0);

    /* redo insert, redo delete */
    assert(lvc_undo_redo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 5);
    assert(lvc_undo_redo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 2);
    assert(lvc_undo_redo(&u, &b) == 0);

    uint8_t out[4] = {0};
    lvc_buf_read(&b, 0, out, 2);
    assert(strcmp((char *)out, "lo") == 0);
    lvc_buf_free(&b);
    lvc_undo_free(&u);
}

static void test_redo_truncated_on_new_edit(void) {
    lvc_buf b;
    lvc_undo u;
    assert(lvc_buf_init(&b, 64) == 0);
    assert(lvc_undo_init(&u, 100) == 0);

    assert(lvc_buf_insert(&b, 0, (const uint8_t *)"abc", 3) == 3);
    assert(lvc_undo_push(&u, LVC_OP_INSERT, 0, (const uint8_t *)"abc", 3, u.next_group++) == 0);
    assert(lvc_undo_undo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 0);
    /* new edit after undo -> redo tail must be discarded */
    assert(lvc_buf_insert(&b, 0, (const uint8_t *)"x", 1) == 1);
    assert(lvc_undo_push(&u, LVC_OP_INSERT, 0, (const uint8_t *)"x", 1, u.next_group++) == 0);
    assert(lvc_undo_redo(&u, &b) == 0);
    assert(lvc_undo_undo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 0);
    lvc_buf_free(&b);
    lvc_undo_free(&u);
}

static void test_grouping(void) {
    lvc_buf b;
    lvc_undo u;
    assert(lvc_buf_init(&b, 64) == 0);
    assert(lvc_undo_init(&u, 100) == 0);

    /* one typing run: 3 chars, same group -> single undo */
    int g = u.next_group;
    const char *run = "abc";
    for (int i = 0; i < 3; i++) {
        assert(lvc_buf_insert(&b, lvc_buf_len(&b), (const uint8_t *)run + i, 1) == 1);
        assert(lvc_undo_push(&u, LVC_OP_INSERT, i, (const uint8_t *)run + i, 1, g) == 0);
    }
    assert(lvc_undo_undo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 0);
    assert(lvc_undo_redo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 3);
    lvc_buf_free(&b);
    lvc_undo_free(&u);
}

static void test_limit_eviction(void) {
    lvc_buf b;
    lvc_undo u;
    assert(lvc_buf_init(&b, 64) == 0);
    assert(lvc_undo_init(&u, 3) == 0);
    for (int i = 0; i < 5; i++) {
        char c = 'a' + i;
        assert(lvc_buf_insert(&b, lvc_buf_len(&b), (const uint8_t *)&c, 1) == 1);
        assert(lvc_undo_push(&u, LVC_OP_INSERT, i, (const uint8_t *)&c, 1, u.next_group++) == 0);
    }
    /* only last 3 ops kept: undo 3 times -> "ab" remains */
    for (int i = 0; i < 3; i++) assert(lvc_undo_undo(&u, &b) == 1);
    assert(lvc_buf_len(&b) == 2);
    assert(lvc_undo_undo(&u, &b) == 0);
    uint8_t out[3] = {0};
    lvc_buf_read(&b, 0, out, 2);
    assert(strcmp((char *)out, "ab") == 0);
    lvc_buf_free(&b);
    lvc_undo_free(&u);
}

int main(void) {
    test_io_roundtrip();
    test_io_missing();
    test_undo_insert_delete();
    test_redo_truncated_on_new_edit();
    test_grouping();
    test_limit_eviction();
    printf("all io/undo tests passed\n");
    return 0;
}
