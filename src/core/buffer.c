#include "buffer.h"
#include <stdlib.h>
#include <string.h>

static int line_index_init(lvc_buf *b) {
    b->line_cap = 64;
    b->line_index = malloc(b->line_cap * sizeof(uint32_t));
    if (!b->line_index) return -1;
    b->line_index[0] = 0;
    b->line_count = 1;
    return 0;
}

static int line_index_reserve(lvc_buf *b, size_t extra) {
    if (b->line_count + extra <= b->line_cap) return 0;
    size_t ncap = b->line_cap;
    while (b->line_count + extra > ncap) ncap *= 2;
    uint32_t *p = realloc(b->line_index, ncap * sizeof(uint32_t));
    if (!p) return -1;
    b->line_index = p;
    b->line_cap = ncap;
    return 0;
}

/* first index with line_index[idx] > off; index 0 (offset 0) is never beyond */
static size_t line_index_upper(const lvc_buf *b, size_t off) {
    const uint32_t *li = b->line_index;
    size_t lo = 1, hi = b->line_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (li[mid] <= off) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* incremental update after inserting len bytes at pos */
static void line_index_inserted(lvc_buf *b, size_t pos, const uint8_t *text,
                                size_t len) {
    size_t k = 0;
    for (size_t i = 0; i < len; i++) if (text[i] == '\n') k++;
    size_t idx = line_index_upper(b, pos);
    if (k > 0 && line_index_reserve(b, k) != 0) return; /* OOM: index stale */
    uint32_t *li = b->line_index;
    memmove(li + idx + k, li + idx, (b->line_count - idx) * sizeof(uint32_t));
    for (size_t i = idx + k; i < b->line_count + k; i++) li[i] += (uint32_t)len;
    size_t w = idx;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') li[w++] = (uint32_t)(pos + i + 1);
    b->line_count += k;
}

/* incremental update after deleting n bytes at pos */
static void line_index_deleted(lvc_buf *b, size_t pos, size_t n) {
    uint32_t *li = b->line_index;
    size_t idx = line_index_upper(b, pos);
    size_t hi = idx;
    while (hi < b->line_count && li[hi] <= pos + n) hi++;
    size_t removed = hi - idx;
    for (size_t i = hi; i < b->line_count; i++)
        li[i - removed] = li[i] - (uint32_t)n;
    b->line_count -= removed;
}

int lvc_buf_init(lvc_buf *b, size_t initial_cap) {
    if (initial_cap < 64) initial_cap = 64;
    b->data = malloc(initial_cap);
    if (!b->data) return -1;
    b->cap = initial_cap;
    b->gap_start = 0;
    b->gap_end = initial_cap;
    b->chars = 0;
    b->line_index = NULL;
    b->line_count = b->line_cap = 0;
    return line_index_init(b);
}

void lvc_buf_free(lvc_buf *b) {
    free(b->data);
    free(b->line_index);
    b->data = NULL;
    b->line_index = NULL;
}

static void gap_move(lvc_buf *b, size_t pos) {
    if (pos == b->gap_start) return;
    if (pos < b->gap_start) {
        size_t len = b->gap_start - pos;
        memmove(b->data + b->gap_end - len, b->data + pos, len);
        b->gap_start = pos;
        b->gap_end -= len;
    } else {
        size_t len = pos - b->gap_start;
        memmove(b->data + b->gap_start, b->data + b->gap_end, len);
        b->gap_start = pos;
        b->gap_end += len;
    }
}

static int gap_reserve(lvc_buf *b, size_t need) {
    if (b->gap_end - b->gap_start >= need) return 0;
    size_t used = lvc_buf_len(b);
    size_t ncap = b->cap * 2;
    while (ncap - used < need + 64) ncap *= 2;
    uint8_t *p = realloc(b->data, ncap);
    if (!p) return -1;
    size_t tail = b->cap - b->gap_end;
    memmove(p + ncap - tail, p + b->gap_end, tail);
    b->gap_end = ncap - tail;
    b->data = p;
    b->cap = ncap;
    return 0;
}

size_t lvc_buf_len(const lvc_buf *b) { return b->cap - (b->gap_end - b->gap_start); }
size_t lvc_buf_line_count(const lvc_buf *b) { return b->line_count; }

size_t lvc_buf_line_start(const lvc_buf *b, size_t line) {
    if (line == 0) return 0;
    if (line >= b->line_count) return lvc_buf_len(b);
    return b->line_index[line];
}

uint8_t lvc_buf_get(lvc_buf *b, size_t pos) {
    gap_move(b, pos);
    return b->data[b->gap_end]; /* pos == gap_start: byte after gap */
}

size_t lvc_utf8_seq_len(uint8_t lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0; /* invalid lead */
}

ptrdiff_t lvc_utf8_validate(const uint8_t *s, size_t len) {
    size_t i = 0, chars = 0;
    while (i < len) {
        size_t n = lvc_utf8_seq_len(s[i]);
        if (n == 0 || i + n > len) return -1;
        for (size_t j = 1; j < n; j++)
            if ((s[i + j] & 0xC0) != 0x80) return -1;
        /* overlong / surrogate / >U+10FFFF checks */
        uint32_t cp = 0;
        switch (n) {
        case 1: cp = s[i]; break;
        case 2: cp = s[i] & 0x1F; break;
        case 3: cp = s[i] & 0x0F; break;
        case 4: cp = s[i] & 0x07; break;
        }
        for (size_t j = 1; j < n; j++) cp = (cp << 6) | (s[i + j] & 0x3F);
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
            (n == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) ||
            cp > 0x10FFFF) return -1;
        i += n; chars++;
    }
    return (ptrdiff_t)chars;
}

bool lvc_utf8_is_boundary(lvc_buf *b, size_t pos) {
    size_t n = lvc_buf_len(b);
    if (pos == 0 || pos >= n) return pos <= n;
    uint8_t c = lvc_buf_get(b, pos);
    return (c & 0xC0) != 0x80; /* not a continuation byte */
}

ptrdiff_t lvc_buf_insert(lvc_buf *b, size_t pos, const uint8_t *text, size_t len) {
    if (pos > lvc_buf_len(b) || !lvc_utf8_is_boundary(b, pos)) return -1;
    if (lvc_utf8_validate(text, len) < 0) return -1;
    if (gap_reserve(b, len) != 0) return -1;
    gap_move(b, pos);
    memcpy(b->data + b->gap_start, text, len);
    b->gap_start += len;
    b->chars += (size_t)lvc_utf8_validate(text, len);
    line_index_inserted(b, pos, text, len);
    return (ptrdiff_t)len;
}

ptrdiff_t lvc_buf_delete(lvc_buf *b, size_t pos, size_t n) {
    size_t total = lvc_buf_len(b);
    if (pos > total || pos + n > total || !lvc_utf8_is_boundary(b, pos)) return -1;
    /* end boundary must also be a char start unless deleting to EOF */
    if (pos + n < total && !lvc_utf8_is_boundary(b, pos + n)) return -1;
    gap_move(b, pos + n);
    b->gap_start -= n; /* widen gap backwards, removing [pos+n-n, pos+n) */
    b->chars -= (size_t)lvc_utf8_validate(b->data + b->gap_start, n); /* removed chars */
    line_index_deleted(b, pos, n);
    return (ptrdiff_t)n;
}

void lvc_buf_read(lvc_buf *b, size_t pos, uint8_t *out, size_t n) {
    gap_move(b, pos);
    /* after gap_move, text from pos is contiguous at [gap_end, cap) */
    memcpy(out, b->data + b->gap_end, n);
}
