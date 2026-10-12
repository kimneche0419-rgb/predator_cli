/* FIM (fill-in-the-middle) prompt building + suggestion logging. */
#ifndef LVC_FIM_H
#define LVC_FIM_H

#include <stddef.h>
#include "../core/buffer.h"

/* Common FIM tokens (Qwen/StarCoder2 style). Prompt = prefix + <|fim_hole|>
 * style; we target Qwen2.5-Coder: <|fim_prefix|>...<|fim_suffix|>...<|fim_middle|> */
typedef struct {
    const char *pre_tok, *suf_tok, *mid_tok;
} lvc_fim_tokens;

extern const lvc_fim_tokens LVC_FIM_QWEN;

/* build a FIM prompt around cursor from buffer. prefix_max/suffix_max cap
 * context bytes. returns malloc'd NUL-terminated prompt, *out_len set.
 * caller frees. */
char *lvc_fim_build_prompt(lvc_buf *b, size_t cursor,
                           size_t prefix_max, size_t suffix_max,
                           size_t *out_len);

/* like build_prompt, but inserts ctx (e.g. cross-file declarations) right
 * after the prefix token, before the buffer prefix. ctx may be NULL. */
char *lvc_fim_build_prompt_ex(lvc_buf *b, size_t cursor,
                              size_t prefix_max, size_t suffix_max,
                              const char *ctx, size_t *out_len);

/* brace-aware suffix length from cursor: extends up to max bytes, ending
 * just past the closer of the block enclosing the cursor. */
size_t lvc_fim_suffix_bound(lvc_buf *b, size_t cursor, size_t max);

/* log an accepted/rejected suggestion as JSONL to path (append).
 * record: ts, lang, ctx (text before cursor), suggestion, accepted.
 * ctx enables DPO pair grouping. returns 0/-1. */
int lvc_fim_log(const char *path, const char *suggestion,
                int accepted, const char *lang, const char *ctx);

/* sanity filter for a suggestion before showing it. ctx is recent text
 * before the cursor (a few lines). rejects: near-empty fragments, parroting
 * of context lines, degenerate line-block repetition. returns 1 keep/0 drop. */
int lvc_fim_sane(const char *suggestion, const char *ctx);

#endif
