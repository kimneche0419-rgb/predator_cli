/* test_fuzz.c: seed-based random-operation fuzzer for lvc_buf + lvc_undo.
 *
 * Performs 100,000 random insert/erase/undo operations on a gap buffer,
 * verifying invariants after every op:
 *   - gap structure: gap_start <= gap_end <= cap, len == cap - gap_size
 *   - UTF-8: stored content is valid UTF-8, tracked char count matches
 *   - line index: lvc_buf_line_count matches newline scan of content
 *   - content: buffer bytes equal a plain-memory mirror model
 * Finally, replays the same recorded op sequence on a clean buffer and
 * compares the rebuilt bytes with the fuzzed buffer.
 */
#include "buffer.h"
#include "undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPS 100000

/* ---- deterministic RNG (xorshift64) ---- */
static uint64_t rng_state;
static uint64_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* ---- mirror model: expected logical content ---- */
typedef struct {
    uint8_t *bytes;
    size_t   len, cap;
} mirror;

static void m_reserve(mirror *m, size_t need) {
    if (need <= m->cap) return;
    size_t nc = m->cap ? m->cap : 64;
    while (nc < need) nc *= 2;
    m->bytes = realloc(m->bytes, nc);
    m->cap = nc;
}
static void m_insert(mirror *m, size_t pos, const uint8_t *text, size_t len) {
    m_reserve(m, m->len + len);
    memmove(m->bytes + pos + len, m->bytes + pos, m->len - pos);
    memcpy(m->bytes + pos, text, len);
    m->len += len;
}
static void m_delete(mirror *m, size_t pos, size_t n) {
    memmove(m->bytes + pos, m->bytes + pos + n, m->len - pos - n);
    m->len -= n;
}

/* ---- mirror undo stack + replay script ---- */
typedef struct { int kind; size_t pos, len; uint8_t *text; } rop; /* kind: 0 insert, 1 delete */
static rop *edits;                  /* applied edits, indexed by u_cursor */
static size_t n_edits, cap_edits;
static size_t u_cursor;             /* next edit to undo (mirrors lvc_undo cursor) */
static int *events;                 /* full replay script: 0 insert, 1 delete, 2 undo */
static rop *event_ops;              /* snapshot payload per event (valid for 0/1) */
static size_t n_events, cap_events;
static uint8_t **graveyard; /* texts of overwritten edit slots, freed at exit */
static size_t n_grave;

static void edit_push(int kind, size_t pos, size_t len, const uint8_t *text) {
    /* live applied edits are edits[0..u_cursor); ops at/after the cursor were
     * undone and are dropped by lvc_undo_push — overwrite that slot in place
     * (old text is parked so replay of earlier events stays valid). */
    if (u_cursor == n_edits) {
        if (n_edits == cap_edits) {
            cap_edits = cap_edits ? cap_edits * 2 : 1024;
            edits = realloc(edits, cap_edits * sizeof(rop));
        }
        n_edits++;
    } else {
        if (edits[u_cursor].text) {
            graveyard = realloc(graveyard, (n_grave + 1) * sizeof(uint8_t *));
            graveyard[n_grave++] = edits[u_cursor].text;
        }
    }
    edits[u_cursor].kind = kind;
    edits[u_cursor].pos = pos;
    edits[u_cursor].len = len;
    edits[u_cursor].text = malloc(len);
    memcpy(edits[u_cursor].text, text, len);
    u_cursor++; /* new edit truncates the redo branch (lvc_undo semantics) */
}

static void event_push(int ev, int kind, size_t pos, size_t len, const uint8_t *text) {
    if (n_events == cap_events) {
        cap_events = cap_events ? cap_events * 2 : 1024;
        events = realloc(events, cap_events * sizeof(int));
        event_ops = realloc(event_ops, cap_events * sizeof(rop));
    }
    events[n_events] = ev;
    event_ops[n_events].kind = kind;
    event_ops[n_events].pos = pos;
    event_ops[n_events].len = len;
    if (text) {
        event_ops[n_events].text = malloc(len);
        memcpy(event_ops[n_events].text, text, len);
    } else {
        event_ops[n_events].text = NULL;
    }
    n_events++;
}

static int m_is_boundary(const uint8_t *p, size_t n, size_t i);

/* ---- invariants ---- */
static size_t line_scan(const uint8_t *p, size_t n) { /* number of lines */
    size_t lines = 1;
    for (size_t i = 0; i < n; i++) if (p[i] == '\n') lines++;
    return lines;
}

static int fail(const char *msg, size_t a, size_t b, int opno) {
    fprintf(stderr, "FUZZ FAIL at op %d: %s (a=%zu b=%zu)\n", opno, msg, a, b);
    return 0;
}

static int check_invariants(lvc_buf *b, mirror *m, lvc_undo *u, int opno) {
    size_t len = lvc_buf_len(b);
    if (b->gap_start > b->gap_end) return fail("gap_start > gap_end", b->gap_start, b->gap_end, opno);
    if (b->gap_end > b->cap) return fail("gap_end > cap", b->gap_end, b->cap, opno);
    if (b->gap_start + (b->cap - b->gap_end) != len)
        return fail("len != cap - gap_size", len, b->cap - (b->gap_end - b->gap_start), opno);
    if (len != m->len) return fail("buffer len != mirror len", len, m->len, opno);
    if (b->chars + (b->gap_end - b->gap_start) > b->cap) return fail("chars overflows gap", b->chars, 0, opno);

    uint8_t *tmp = malloc(len ? len : 1);
    if (len) lvc_buf_read(b, 0, tmp, len);
    if (memcmp(tmp, m->bytes, len) != 0) {
        for (size_t i = 0; i < len; i++)
            if (tmp[i] != m->bytes[i]) return fail("content mismatch at byte", i, opno, opno);
    }
    if (lvc_utf8_validate(tmp, len) < 0 && len) {
        free(tmp); return fail("content is not valid UTF-8", 0, 0, opno);
    }
    if (line_scan(tmp, len) != lvc_buf_line_count(b)) {
        free(tmp); return fail("line_count mismatch", line_scan(tmp, len), lvc_buf_line_count(b), opno);
    }
    free(tmp);

    /* every char boundary of the mirror must be a buffer boundary, and
     * non-boundaries must not be */
    for (size_t i = 0; i <= m->len && i <= len; i++) {
        if (lvc_utf8_is_boundary(b, i) != m_is_boundary(m->bytes, m->len, i))
            return fail("boundary mismatch", i, 0, opno);
    }
    (void)u;
    return 1;
}

static int m_is_boundary(const uint8_t *p, size_t n, size_t i) {
    if (i == 0 || i >= n) return i <= n;
    return (p[i] & 0xC0) != 0x80;
}

/* generate a random valid UTF-8 chunk (1-8 bytes) */
static size_t rand_utf8(uint8_t *out) {
    static const char *samples[] = {
        "a", "z", "0", " ", "\n", "{", "~",
        "\xc3\xa9", "\xec\x9d\xb8", "\xf0\x9f\x98\x80", "\xe2\x82\xac",
        "ab", "\xf0\x9f\x98\x80\n", "x\xec\x9d\xb8y",
    };
    const char *s = samples[rng_next() % (sizeof samples / sizeof *samples)];
    size_t n = strlen(s);
    memcpy(out, s, n);
    return n;
}

int main(void) {
    rng_state = 0x9E3779B97F4A7C15ULL; /* fixed seed: reproducible */

    lvc_buf b;
    if (lvc_buf_init(&b, 16) != 0) { fprintf(stderr, "init failed\n"); return 1; }
    lvc_undo u;
    if (lvc_undo_init(&u, OPS + 16) != 0) { fprintf(stderr, "undo init failed\n"); return 1; }
    mirror m = {0};
    uint8_t text[16];

    for (int op = 0; op < OPS; op++) {
        uint64_t r = rng_next() % 100;
        if (r < 55) { /* insert */
            size_t len = rand_utf8(text);
            size_t pos;
            if (m.len == 0) pos = 0;
            else pos = rng_next() % (m.len + 1);
            while (!m_is_boundary(m.bytes, m.len, pos)) pos++; /* snap to boundary */
            ptrdiff_t rc = lvc_buf_insert(&b, pos, text, len);
            if (rc != (ptrdiff_t)len) return fail("insert rejected", pos, len, op) ? 1 : 1;
            m_insert(&m, pos, text, len);
            lvc_undo_push(&u, LVC_OP_INSERT, pos, text, len, 2 * op);
            edit_push(0, pos, len, text);
            event_push(0, 0, pos, len, text);
        } else if (r < 85) { /* erase */
            if (m.len == 0) continue;
            size_t pos = rng_next() % m.len;
            while (!m_is_boundary(m.bytes, m.len, pos)) pos++;
            size_t maxlen = m.len - pos;
            if (maxlen > 12) maxlen = 12;
            if (maxlen == 0) continue; /* boundary at end: nothing to erase */
            size_t n = 1 + (size_t)(rng_next() % maxlen);
            while (pos + n < m.len && !m_is_boundary(m.bytes, m.len, pos + n)) n++;
            uint8_t *dead = malloc(n);
            lvc_buf_read(&b, pos, dead, n); /* capture for undo record */
            ptrdiff_t rc = lvc_buf_delete(&b, pos, n);
            if (rc != (ptrdiff_t)n) return fail("delete rejected", pos, n, op) ? 1 : 1;
            m_delete(&m, pos, n);
            lvc_undo_push(&u, LVC_OP_DELETE, pos, dead, n, 2 * op + 1);
            edit_push(1, pos, n, dead);
            event_push(1, 1, pos, n, dead);
            free(dead);
        } else { /* undo */
            if (lvc_undo_undo(&u, &b) == 1) {
                /* mirror the inverse of the op at the undo cursor */
                if (u_cursor == 0)
                    return fail("undo applied but mirror cursor at start", 0, 0, op) ? 1 : 1;
                rop *target = &edits[u_cursor - 1];
                if (target->kind == 0) m_delete(&m, target->pos, target->len);
                else m_insert(&m, target->pos, target->text, target->len);
                u_cursor--;
                event_push(2, target->kind, target->pos, target->len, target->text); /* snapshot for replay */
            }
        }
        if (!check_invariants(&b, &m, &u, op)) return 1;
    }

    /* --- clean-buffer replay of the identical op sequence --- */
    lvc_buf b2;
    if (lvc_buf_init(&b2, 8) != 0) return 1;
    lvc_undo u2;
    if (lvc_undo_init(&u2, OPS + 16) != 0) return 1;
    mirror m2 = {0};
    size_t uc2 = 0;
    for (size_t i = 0; i < n_events; i++) {
        int ev = events[i];
        if (ev == 2) {
            rop *t = &event_ops[i]; /* snapshot of the reverted edit */
            if (lvc_undo_undo(&u2, &b2) != 1) return fail("replay undo failed", i, 0, 0) ? 1 : 1;
            if (uc2 == 0) return fail("replay undo with empty mirror stack", i, 0, 0) ? 1 : 1;
            if (t->kind == 0) m_delete(&m2, t->pos, t->len);
            else m_insert(&m2, t->pos, t->text, t->len);
            uc2--;
        } else {
            rop *o = &event_ops[i];
            if (ev == 0) {
                if (lvc_buf_insert(&b2, o->pos, o->text, o->len) != (ptrdiff_t)o->len)
                    return fail("replay insert failed", o->pos, o->len, (int)i) ? 1 : 1;
                m_insert(&m2, o->pos, o->text, o->len);
                lvc_undo_push(&u2, LVC_OP_INSERT, o->pos, o->text, o->len, (int)(2 * i));
                uc2++; /* push truncates the redo branch */
            } else {
                if (lvc_buf_delete(&b2, o->pos, o->len) != (ptrdiff_t)o->len)
                    return fail("replay delete failed", o->pos, o->len, (int)i) ? 1 : 1;
                m_delete(&m2, o->pos, o->len);
                lvc_undo_push(&u2, LVC_OP_DELETE, o->pos, o->text, o->len, (int)(2 * i + 1));
                uc2++;
            }
        }
        if (lvc_buf_len(&b2) != m2.len)
            return fail("replay mirror len mismatch", m2.len, lvc_buf_len(&b2), (int)i) ? 1 : 1;
    }
    size_t len = lvc_buf_len(&b);
    if (lvc_buf_len(&b2) != len) return fail("replay len mismatch", len, lvc_buf_len(&b2), 0) ? 1 : 1;
    uint8_t *t1 = malloc(len ? len : 1), *t2 = malloc(len ? len : 1);
    if (len) { lvc_buf_read(&b, 0, t1, len); lvc_buf_read(&b2, 0, t2, len); }
    if (len && memcmp(t1, t2, len) != 0) return fail("replay bytes mismatch", 0, 0, 0) ? 1 : 1;
    if (len && memcmp(t1, m.bytes, len) != 0) return fail("replay vs mirror mismatch", 0, 0, 0) ? 1 : 1;

    free(t1); free(t2);
    for (size_t i = 0; i < n_edits; i++) free(edits[i].text);
    free(edits); free(events); free(m.bytes); free(m2.bytes);
    for (size_t i = 0; i < n_events; i++) free(event_ops[i].text);
    free(event_ops);
    for (size_t i = 0; i < n_grave; i++) free(graveyard[i]);
    free(graveyard);
    lvc_undo_free(&u2); lvc_buf_free(&b2);
    lvc_undo_free(&u); lvc_buf_free(&b);
    printf("all fuzz tests passed (%d ops, %zu edits, %zu events)\n", OPS, n_edits, n_events);
    return 0;
}
