#include "../src/core/diff.h"
#include "../src/ai/lsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static void test_diff_apply_roundtrip(void) {
    const char *old_t = "int main(void) {\n    int x = 1;\n    return 0;\n}\n";
    const char *new_t = "int main(void) {\n    int x = 2;\n    printf(\"x\");\n    return 0;\n}\n";
    lvc_diff d;
    assert(lvc_diff_lines(old_t, new_t, &d) == 0);
    assert(d.count >= 1);
    char *applied = lvc_diff_apply(old_t, &d);
    assert(strcmp(applied, new_t) == 0);
    free(applied);
    lvc_diff_free(&d);
}

static void test_diff_no_change(void) {
    const char *t = "a\nb\nc\n";
    lvc_diff d;
    assert(lvc_diff_lines(t, t, &d) == 0);
    assert(d.count == 0);
    char *applied = lvc_diff_apply(t, &d);
    assert(strcmp(applied, t) == 0);
    free(applied);
    lvc_diff_free(&d);
}

static void test_diff_replace_all(void) {
    lvc_diff d;
    assert(lvc_diff_lines("x\n", "y\nz\n", &d) == 0);
    char *applied = lvc_diff_apply("x\n", &d);
    assert(strcmp(applied, "y\nz\n") == 0);
    free(applied);
    lvc_diff_free(&d);
}

static void test_framing_roundtrip_pipe(void) {
    /* inproc: write to a pipe, read back */
    int fds[2];
    assert(pipe(fds) == 0);
    const char *body = "{\"jsonrpc\":\"2.0\"}";
    assert(lvc_lsp_write_frame(fds[1], body) == 0);
    close(fds[1]);
    char *got = NULL;
    assert(lvc_lsp_read_frame(fds[0], &got) == 0);
    assert(strcmp(got, body) == 0);
    free(got);
    close(fds[0]);
}

int main(void) {
    test_diff_apply_roundtrip();
    test_diff_no_change();
    test_diff_replace_all();
    test_framing_roundtrip_pipe();
    printf("diff/lsp-framing tests passed\n");
    return 0;
}
