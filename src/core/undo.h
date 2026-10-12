/* undo/redo: command-based history over lvc_buf. Grouping (coalescing
 * consecutive char inserts) keeps memory O(edits) without full snapshots. */
#ifndef LVC_UNDO_H
#define LVC_UNDO_H

#include "buffer.h"

typedef enum { LVC_OP_INSERT, LVC_OP_DELETE } lvc_op_kind;

typedef struct lvc_op {
    lvc_op_kind kind;
    size_t   pos;
    size_t   len;
    uint8_t *text;        /* owned; NULL for delete */
    int      group_id;    /* same id = coalesced (e.g. typed run) */
    struct lvc_op *prev, *next;
} lvc_op;

typedef struct {
    lvc_op *head;         /* sentinel ring head */
    lvc_op *cursor;       /* next op to undo; == head->next means nothing to undo */
    size_t  limit;        /* max ops kept */
    size_t  count;
    int     next_group;
} lvc_undo;

int  lvc_undo_init(lvc_undo *u, size_t limit);
void lvc_undo_free(lvc_undo *u);

/* record an already-applied edit. text is copied. for LVC_OP_DELETE pass the
 * deleted bytes (read them from the buffer before deleting); for LVC_OP_INSERT
 * pass the inserted bytes. group_id: same value coalesces (typing run). */
int lvc_undo_push(lvc_undo *u, lvc_op_kind kind, size_t pos, const uint8_t *text, size_t len, int group_id);

/* convenience: deletes b[pos..pos+len) and records the op with the captured
 * text so undo can restore it. */
ptrdiff_t lvc_undo_delete(lvc_undo *u, lvc_buf *b, size_t pos, size_t len, int group_id);

/* apply inverse of last op to b. returns 1 if applied, 0 if nothing to undo. */
int lvc_undo_undo(lvc_undo *u, lvc_buf *b);
/* returns 1 if applied, 0 if nothing to redo. */
int lvc_undo_redo(lvc_undo *u, lvc_buf *b);

#endif
