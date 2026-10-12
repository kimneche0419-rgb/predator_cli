/* line-based diff via LCS DP. Buffers are source-code sized (<= a few MB);
 * O(n*m) table is capped and falls back to "replace whole" beyond that. */
#ifndef LVC_DIFF_H
#define LVC_DIFF_H

#include <stddef.h>

typedef struct {
    size_t old_start, new_start;  /* 0-based line numbers */
    size_t del_count, ins_count;
    char **del_lines;             /* borrowed; NULL if none */
    char **ins_lines;
} lvc_hunk;

typedef struct {
    lvc_hunk *hunks;
    size_t count;
    char **owned;      /* all distinct line strings (owned) */
    size_t owned_count;
} lvc_diff;

/* diff two texts ('\n'-separated). returns 0 or -1. */
int lvc_diff_lines(const char *old_text, const char *new_text, lvc_diff *d);
void lvc_diff_free(lvc_diff *d);

/* apply diff's hunks to old_text -> new_text (malloc, NUL-terminated). */
char *lvc_diff_apply(const char *old_text, const lvc_diff *d);

#endif
