#include "../src/ai/lsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 256) == 0);
    const char *src = "int main(void) {\n    return BAD;\n}\n";
    assert(lvc_buf_insert(&b, 0, (const uint8_t *)src, strlen(src)) > 0);

    lvc_lsp lsp;
    char *argv[] = { "python3", "tests/mock_lsp.py", NULL };
    assert(lvc_lsp_start(&lsp, argv, "file:///tmp", "main.c", &b) == 0);
    assert(lsp.pid > 0);

    /* didChange triggers diagnostics */
    assert(lvc_lsp_notify_change(&lsp, "main.c", &b) == 0);
    /* poll until diagnostics captured (bounded) */
    int captured = 0;
    for (int i = 0; i < 50 && !captured; i++) {
        usleep(20000);
        captured = lvc_lsp_poll(&lsp) && lsp.last_diagnostics &&
                   strstr(lsp.last_diagnostics, "undeclared identifier");
    }
    assert(captured);
    printf("diagnostics: %.60s...\n", lsp.last_diagnostics);

    /* hover request/response */
    char *resp = lvc_lsp_request(&lsp, "textDocument/hover",
        "{\"textDocument\":{\"uri\":\"file:///tmp/main.c\"},\"position\":{\"line\":0,\"character\":4}}");
    assert(resp);
    assert(strstr(resp, "int main(void)"));
    free(resp);

    assert(lvc_lsp_stop(&lsp) == 0);
    lvc_buf_free(&b);
    printf("lsp e2e tests passed\n");
    return 0;
}
