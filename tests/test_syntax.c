#include "../src/core/syntax.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    lvc_buf b;
    assert(lvc_buf_init(&b, 256) == 0);
    const char *src =
        "#include <stdio.h>\n"
        "// comment\n"
        "int main(void) {\n"
        "  const char *s = \"hi\";\n"
        "  return 42;\n"
        "}\n";
    size_t len = strlen(src);
    assert(lvc_buf_insert(&b, 0, (const uint8_t *)src, len) == (ptrdiff_t)len);

    lvc_syntax s;
    assert(lvc_syntax_init(&s) == 0);
    assert(lvc_syntax_parse(&s, &b) == 0);

    lvc_span spans[256];
    size_t n = lvc_syntax_spans(&s, spans, 256);
    printf("spans=%zu\n", n);
    assert(n > 0);

    /* sanity: comment span covers "// comment" */
    bool have_comment = false, have_string = false, have_number = false, have_keyword = false;
    for (size_t i = 0; i < n; i++) {
        if (spans[i].kind == LVC_TK_COMMENT) have_comment = true;
        if (spans[i].kind == LVC_TK_STRING) have_string = true;
        if (spans[i].kind == LVC_TK_NUMBER) have_number = true;
        if (spans[i].kind == LVC_TK_KEYWORD) have_keyword = true;
    }
    assert(have_comment && have_string && have_number && have_keyword);

    /* incremental edit: change 42 -> 7, reparse */
    const char *needle = "42";
    size_t pos = strstr(src, needle) - src;
    assert(lvc_buf_delete(&b, pos, 2) == 2);
    lvc_syntax_edit(&s, &b, (uint32_t)pos, (uint32_t)(pos + 2), (uint32_t)pos);
    assert(lvc_syntax_parse(&s, &b) == 0);
    n = lvc_syntax_spans(&s, spans, 256);
    assert(n > 0);
    printf("post-edit spans=%zu\n", n);

    lvc_syntax_free(&s);
    lvc_buf_free(&b);
    printf("syntax tests passed\n");
    return 0;
}
