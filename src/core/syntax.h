/* syntax highlighting via tree-sitter: highlight a C source buffer into
 * token classes the renderer maps to colors. */
#ifndef LVC_SYNTAX_H
#define LVC_SYNTAX_H

#include "buffer.h"
#include "../../vendor/tree-sitter/lib/include/tree_sitter/api.h"

typedef enum {
    LVC_TK_PLAIN = 0,
    LVC_TK_KEYWORD,
    LVC_TK_STRING,
    LVC_TK_COMMENT,
    LVC_TK_NUMBER,
    LVC_TK_FUNCTION,
    LVC_TK_TYPE,
    LVC_TK_PREPROC,
} lvc_token_kind;

typedef struct { uint32_t start; uint32_t end; lvc_token_kind kind; } lvc_span;

typedef struct {
    TSParser *parser;
    TSTree   *tree;
} lvc_syntax;

int  lvc_syntax_init(lvc_syntax *s);           /* C grammar */
void lvc_syntax_free(lvc_syntax *s);

/* reparse full buffer (O(n); incremental edit via lvc_syntax_edit below) */
int  lvc_syntax_parse(lvc_syntax *s, lvc_buf *b);

/* incremental reparse after an edit at byte range [start,start+old_end_removed)
 * with new_end bytes now present. */
void lvc_syntax_edit(lvc_syntax *s, lvc_buf *b, uint32_t start,
                     uint32_t old_end, uint32_t new_end);

/* collect highlight spans (max `max`). returns count written. */
size_t lvc_syntax_spans(lvc_syntax *s, lvc_span *out, size_t max);

#endif
