#include "undo.h"
#include <stdlib.h>
#include <string.h>

typedef struct lvc_op_node { lvc_op op; } unused_t;

int lvc_undo_init(lvc_undo *u, size_t limit) {
    u->head = malloc(sizeof(lvc_op));
    if (!u->head) return -1;
    u->head->prev = u->head->next = u->head;
    u->head->text = NULL;
    u->cursor = u->head->next; /* == head: empty */
    u->limit = limit ? limit : 1000;
    u->count = 0;
    u->next_group = 1;
    return 0;
}

static void op_free(lvc_op *o) {
    free(o->text);
    free(o);
}

static void drop_from(lvc_undo *u, lvc_op *first) {
    /* unlink and free [first, head) circular region */
    lvc_op *o = first;
    while (o != u->head) {
        lvc_op *next = o->next;
        o->prev->next = o->next;
        o->next->prev = o->prev;
        op_free(o);
        if (u->count) u->count--;
        o = next;
    }
}

void lvc_undo_free(lvc_undo *u) {
    drop_from(u, u->head->next);
    free(u->head);
    u->head = NULL;
}

int lvc_undo_push(lvc_undo *u, lvc_op_kind kind, size_t pos, const uint8_t *text, size_t len, int group_id) {
    /* any new edit truncates the redo tail (ops from cursor to head) */
    drop_from(u, u->cursor);

    lvc_op *o = malloc(sizeof(lvc_op));
    if (!o) return -1;
    o->kind = kind;
    o->pos = pos;
    o->len = len;
    o->group_id = group_id;
    o->text = NULL;
    if (len) { /* both kinds carry payload: insert text / deleted text for undo */
        o->text = malloc(len);
        if (!o->text) { free(o); return -1; }
        memcpy(o->text, text, len);
    }
    /* insert before head (most recent at tail) */
    o->prev = u->head->prev;
    o->next = u->head;
    u->head->prev->next = o;
    u->head->prev = o;
    u->cursor = o->next;
    u->count++;
    if (u->count > u->limit) { /* evict single oldest */
        lvc_op *old = u->head->next;
        old->prev = u->head;
        old->next->prev = u->head;
        u->head->next = old->next;
        op_free(old);
        u->count--;
    }
    return 0;
}

ptrdiff_t lvc_undo_delete(lvc_undo *u, lvc_buf *b, size_t pos, size_t len, int group_id) {
    uint8_t stackbuf[256];
    uint8_t *heap = NULL;
    uint8_t *txt = stackbuf;
    if (len > sizeof stackbuf) {
        heap = malloc(len);
        if (!heap) return -1;
        txt = heap;
    }
    lvc_buf_read(b, pos, txt, len);
    ptrdiff_t r = lvc_buf_delete(b, pos, len);
    if (r < 0 || lvc_undo_push(u, LVC_OP_DELETE, pos, txt, len, group_id) != 0) {
        free(heap);
        return -1;
    }
    free(heap);
    return r;
}

static int revert(lvc_buf *b, lvc_op *o) {
    if (o->kind == LVC_OP_INSERT) {
        ptrdiff_t r = lvc_buf_delete(b, o->pos, o->len);
        if (r < 0) return -1;
    } else {
        ptrdiff_t r = lvc_buf_insert(b, o->pos, o->text, o->len);
        if (r < 0) return -1;
    }
    return 0;
}

int lvc_undo_undo(lvc_undo *u, lvc_buf *b) {
    if (u->cursor == u->head->next) return 0;
    lvc_op *o = u->cursor->prev;
    int group = o->group_id;
    do {
        if (revert(b, o) != 0) return -1;
        o = o->prev;
    } while (o != u->head && o->group_id == group);
    u->cursor = o->next;
    return 1;
}

int lvc_undo_redo(lvc_undo *u, lvc_buf *b) {
    if (u->cursor == u->head) return 0;
    lvc_op *o = u->cursor;
    int group = o->group_id;
    do {
        if (o->kind == LVC_OP_INSERT) {
            if (lvc_buf_insert(b, o->pos, o->text, o->len) < 0) return -1;
        } else {
            if (lvc_buf_delete(b, o->pos, o->len) < 0) return -1;
        }
        o = o->next;
    } while (o != u->head && o->group_id == group);
    u->cursor = o;
    return 1;
}
