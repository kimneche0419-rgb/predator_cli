#include "syntax.h"
#include "../../vendor/tree-sitter-c/src/tree_sitter/parser.h"
#include <stdlib.h>
#include <string.h>

/* TSLanguage extern from grammar */
const TSLanguage *tree_sitter_c(void);

static bool captured_keyword(const char *node_type);

int lvc_syntax_init(lvc_syntax *s) {
    s->parser = ts_parser_new();
    if (!ts_parser_set_language(s->parser, tree_sitter_c())) {
        ts_parser_delete(s->parser);
        return -1;
    }
    s->tree = NULL;
    return 0;
}

void lvc_syntax_free(lvc_syntax *s) {
    if (s->tree) ts_tree_delete(s->tree);
    ts_parser_delete(s->parser);
}

static const char *read_cb(void *payload, uint32_t byte_index, TSPoint pos, uint32_t *bytes_read) {
    lvc_buf *b = payload;
    (void)pos;
    static uint8_t chunk[1];
    /* tree-sitter reads forward in small calls; feed one byte at a time is
     * slow, so read a small window when possible */
    size_t n = lvc_buf_len(b);
    if (byte_index >= n) { *bytes_read = 0; return NULL; }
    size_t avail = n - byte_index;
    static uint8_t window[4096];
    size_t take = avail < sizeof window ? avail : sizeof window;
    lvc_buf_read(b, byte_index, window, take);
    (void)chunk;
    *bytes_read = (uint32_t)take;
    return (const char *)window;
}

int lvc_syntax_parse(lvc_syntax *s, lvc_buf *b) {
    TSTree *t = ts_parser_parse(s->parser, s->tree,
        (TSInput){ .payload = b, .read = read_cb, .encoding = TSInputEncodingUTF8 });
    if (!t) return -1;
    if (s->tree) ts_tree_delete(s->tree);
    s->tree = t;
    return 0;
}

void lvc_syntax_edit(lvc_syntax *s, lvc_buf *b, uint32_t start,
                     uint32_t old_end, uint32_t new_end) {
    if (!s->tree) { lvc_syntax_parse(s, b); return; }
    /* byte offsets -> points: walk buffer (renderer caches points; simple pass here) */
    TSPoint p_start = {0, 0}, p_old = {0, 0}, p_new = {0, 0};
    uint32_t line = 0, col = 0;
    for (uint32_t i = 0; i < new_end || i < old_end; ) {
        if (i == start) p_start = (TSPoint){line, col};
        if (i == old_end) p_old = (TSPoint){line, col};
        if (i == new_end) p_new = (TSPoint){line, col};
        if (i >= new_end && i >= old_end) break;
        uint8_t c = lvc_buf_get(b, i);
        if (c == '\n') { line++; col = 0; } else col++;
        i++;
    }
    if (old_end == new_end) { /* pure insert: old_end point == start */
        p_old = p_new = (new_end >= old_end) ? p_start : p_old;
    }
    TSInputEdit e = {
        .start_byte = start, .old_end_byte = old_end, .new_end_byte = new_end,
        .start_point = p_start, .old_end_point = p_old, .new_end_point = p_new,
    };
    ts_tree_edit(s->tree, &e);
    lvc_syntax_parse(s, b);
}

static lvc_token_kind classify(const TSNode node, const char *type) {
    if (strcmp(type, "comment") == 0) return LVC_TK_COMMENT;
    if (strcmp(type, "string_literal") == 0 || strcmp(type, "char_literal") == 0 ||
        strcmp(type, "system_lib_string") == 0) return LVC_TK_STRING;
    if (strcmp(type, "number_literal") == 0) return LVC_TK_NUMBER;
    if (strcmp(type, "preproc_def") == 0 || strcmp(type, "preproc_function_def") == 0 ||
        strcmp(type, "preproc_call") == 0 || strcmp(type, "#include") == 0 ||
        strcmp(type, "#define") == 0) return LVC_TK_PREPROC;
    if (strcmp(type, "identifier") == 0) {
        /* node text needed for keyword/type/function decisions: caller context */
        return LVC_TK_PLAIN;
    }
    if (captured_keyword(type)) return LVC_TK_KEYWORD;
    if (strcmp(type, "primitive_type") == 0 || strcmp(type, "type_identifier") == 0)
        return LVC_TK_TYPE;
    if (strcmp(type, "function_declarator") == 0) return LVC_TK_FUNCTION;
    return LVC_TK_PLAIN;
}

static bool captured_keyword(const char *t) {
    static const char *kws[] = {
        "if","else","while","for","do","switch","case","default","break",
        "continue","return","goto","sizeof","static","extern","const","volatile",
        "register","inline","restrict","struct","union","enum","typedef",
    };
    for (size_t i = 0; i < sizeof kws / sizeof *kws; i++)
        if (strcmp(t, kws[i]) == 0) return true;
    return false;
}

size_t lvc_syntax_spans(lvc_syntax *s, lvc_span *out, size_t max) {
    if (!s->tree) return 0;
    size_t n = 0;
    uint32_t count = ts_tree_root_node(s->tree).id ? 0 : 0; /* walk below */
    (void)count;
    /* iterative DFS */
    typedef struct { TSNode node; uint32_t child; } frame;
    frame *stack = malloc(4096 * sizeof(frame));
    if (!stack) return 0;
    size_t sp = 0;
    TSNode root = ts_tree_root_node(s->tree);
    stack[sp++] = (frame){ root, 0 };
    while (sp && n < max) {
        frame *f = &stack[sp - 1];
        if (f->child == 0) {
            const char *type = ts_node_type(f->node);
            lvc_token_kind k = classify(f->node, type);
            bool leaf = ts_node_child_count(f->node) == 0;
            if (k != LVC_TK_PLAIN || leaf) {
                TSPoint s0 = ts_node_start_point(f->node);
                TSPoint e0 = ts_node_end_point(f->node);
                if (k != LVC_TK_PLAIN) {
                    out[n++] = (lvc_span){
                        /* byte offsets: start_byte available directly */
                        ts_node_start_byte(f->node), ts_node_end_byte(f->node), k };
                }
                (void)s0; (void)e0;
            }
        }
        if (f->child < ts_node_child_count(f->node)) {
            TSNode c = ts_node_child(f->node, f->child++);
            if (ts_node_child_count(c) > 0 && sp < 4095)
                stack[sp++] = (frame){ c, 0 };
            else {
                const char *type = ts_node_type(c);
                lvc_token_kind k = classify(c, type);
                if (k != LVC_TK_PLAIN)
                    out[n++] = (lvc_span){ ts_node_start_byte(c), ts_node_end_byte(c), k };
            }
        } else {
            sp--;
        }
    }
    free(stack);
    return n;
}
