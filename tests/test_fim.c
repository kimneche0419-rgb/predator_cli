#include "../src/ai/fim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 128) == 0);
    const char *src = "int foo(void) {\n    \n}\n"; /* cursor on indented blank line */
    assert(lvc_buf_insert(&b, 0, (const uint8_t *)src, strlen(src)) > 0);

    size_t cursor = strlen("int foo(void) {\n    "); /* after spaces */
    size_t plen = 0;
    char *prompt = lvc_fim_build_prompt(&b, cursor, 4096, 4096, &plen);
    assert(prompt);
    printf("prompt=[%s]\n", prompt);
    /* structure: prefix contains head, suffix contains tail, middle token last */
    const char *suf = strstr(prompt, "<|fim_suffix|>");
    const char *mid = strstr(prompt, "<|fim_middle|>");
    assert(suf && mid && suf < mid);
    assert(strstr(prompt, "int foo(void)") == strstr(prompt, "<|fim_prefix|>") + 14);
    assert(strstr(prompt, "}") > mid || suf < mid);
    free(prompt);

    /* capped context */
    prompt = lvc_fim_build_prompt(&b, cursor, 5, 3, &plen);
    assert(prompt);
    const char *pre = strstr(prompt, "<|fim_prefix|>");
    assert(strncmp(pre + 14, "\n    ", 5) == 0); /* last 5 bytes before cursor */
    free(prompt);

    /* position 1 is an ASCII boundary (valid); a continuation byte is not */
    assert(lvc_fim_build_prompt(&b, 1, 10, 10, &plen) != NULL);
    lvc_buf_free(&b);

    /* logging */
    const char *path = "/tmp/lvc_fim_log.jsonl";
    remove(path);
    assert(lvc_fim_log(path, "return 42;\n", 1, "c", "int main() {\n") == 0);
    assert(lvc_fim_log(path, "bad \"quote\"", 0, "c", NULL) == 0);
    FILE *f = fopen(path, "r");
    char line1[256], line2[256];
    assert(fgets(line1, sizeof line1, f) && fgets(line2, sizeof line2, f));
    fclose(f);
    assert(strstr(line1, "\"accepted\":true") && strstr(line1, "return 42;\\n"));
    assert(strstr(line1, "\"ctx\":\"int main() {\\n\""));
    assert(strstr(line2, "bad \\\"quote\\\"") && strstr(line2, "\"accepted\":false"));
    assert(strstr(line2, "\"ctx\":\"\""));
    remove(path);

    /* missing parent dir is created on first write */
    const char *nested = "/tmp/lvc_fim_log_dir/x.jsonl";
    remove(nested); rmdir("/tmp/lvc_fim_log_dir");
    assert(lvc_fim_log(nested, "ok", 1, "c", NULL) == 0);
    f = fopen(nested, "r");
    assert(f && fgets(line1, sizeof line1, f) && strstr(line1, "\"accepted\":true"));
    fclose(f);
    remove(nested); rmdir("/tmp/lvc_fim_log_dir");

    /* sanity filter */
    assert(lvc_fim_sane("return a + b;", "int foo(void) {\n    x = 1;\n") == 1);
    assert(lvc_fim_sane("if", "int foo(void) {\n") == 0);            /* fragment */
    assert(lvc_fim_sane("", "x") == 0);                               /* empty */
    assert(lvc_fim_sane("   \n  ", "x") == 0);                        /* whitespace */
    assert(lvc_fim_sane("    x = 1;", "int foo(void) {\n    x = 1;\n") == 0); /* parrot */
    assert(lvc_fim_sane("a;\na;\na;\n", "int foo(void) {\n") == 0);   /* 1-line loop */
    assert(lvc_fim_sane("PropertyParams\n    a: int\nPropertyParams\n    a: int\n"
                        "PropertyParams\n    a: int\n", "x") == 0);  /* 2-line loop */

    printf("fim tests passed\n");
    return 0;
}
