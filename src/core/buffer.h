/* lvc core: gap-buffer text buffer with UTF-8 support.
 * Invariants:
 *  - text is stored as bytes; gap is [gap_start, gap_end)
 *  - all public byte offsets are logical (pre-gap) positions
 *  - UTF-8 boundaries: inserts must be at char boundaries (utf8 verified)
 */
#ifndef LVC_BUFFER_H
#define LVC_BUFFER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t   cap;         /* physical capacity */
    size_t   gap_start;   /* gap begin (byte offset in buffer) */
    size_t   gap_end;     /* gap end */
    size_t   chars;       /* number of UTF-8 code points */
    uint32_t *line_index; /* flat array: byte offset of each line start, ascending */
    size_t   line_count;  /* number of entries in line_index */
    size_t   line_cap;
} lvc_buf;

int   lvc_buf_init(lvc_buf *b, size_t initial_cap);
void  lvc_buf_free(lvc_buf *b);

size_t lvc_buf_len(const lvc_buf *b);          /* bytes (excluding gap) */
size_t lvc_buf_line_count(const lvc_buf *b);   /* lines >= 1 */
size_t lvc_buf_line_start(const lvc_buf *b, size_t line); /* byte offset of line */

/* byte at logical offset (O(1) amortized: moves gap) */
uint8_t lvc_buf_get(lvc_buf *b, size_t pos);

/* insert UTF-8 text of len bytes at logical byte offset pos.
 * returns bytes inserted, or -1 if pos is not a char boundary / invalid UTF-8. */
ptrdiff_t lvc_buf_insert(lvc_buf *b, size_t pos, const uint8_t *text, size_t len);

/* delete n bytes starting at logical offset pos. returns bytes deleted, -1 on
 * non-boundary start. */
ptrdiff_t lvc_buf_delete(lvc_buf *b, size_t pos, size_t n);

/* read logical range [pos, pos+n) into out (n bytes). handles gap split. */
void lvc_buf_read(lvc_buf *b, size_t pos, uint8_t *out, size_t n);

/* UTF-8 helpers (also usable standalone) */
bool lvc_utf8_is_boundary(lvc_buf *b, size_t pos);
size_t   lvc_utf8_seq_len(uint8_t lead);
ptrdiff_t lvc_utf8_validate(const uint8_t *s, size_t len); /* -1 invalid, else char count */

#endif
