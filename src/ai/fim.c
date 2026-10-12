#include "fim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

const lvc_fim_tokens LVC_FIM_QWEN = {
    "<|fim_prefix|>", "<|fim_suffix|>", "<|fim_middle|>",
};

char *lvc_fim_build_prompt(lvc_buf *b, size_t cursor,
                           size_t prefix_max, size_t suffix_max,
                           size_t *out_len) {
    return lvc_fim_build_prompt_ex(b, cursor, prefix_max, suffix_max, NULL,
                                   out_len);
}

char *lvc_fim_build_prompt_ex(lvc_buf *b, size_t cursor,
                              size_t prefix_max, size_t suffix_max,
                              const char *ctx, size_t *out_len) {
    size_t n = lvc_buf_len(b);
    if (cursor > n || !lvc_utf8_is_boundary(b, cursor)) return NULL;

    size_t ctx_len = ctx ? strlen(ctx) : 0;
    size_t total = n + ctx_len + 128; /* tokens + slack */
    char *out = malloc(total);
    if (!out) return NULL;
    size_t w = 0;

    const lvc_fim_tokens *t = &LVC_FIM_QWEN;
    w += (size_t)snprintf(out + w, total - w, "%s", t->pre_tok);
    if (ctx_len) {
        memcpy(out + w, ctx, ctx_len);
        w += ctx_len;
    }

    /* prefix: last prefix_max bytes before cursor */
    size_t prefix_len = cursor < prefix_max ? cursor : prefix_max;
    /* snap start to char boundary */
    size_t pstart = cursor - prefix_len;
    while (pstart < cursor && !lvc_utf8_is_boundary(b, pstart)) pstart++;
    prefix_len = cursor - pstart;

    /* suffix: first suffix_max bytes after cursor */
    size_t suffix_len = n - cursor;
    if (suffix_len > suffix_max) suffix_len = suffix_max;
    size_t send = cursor + suffix_len;
    while (send > cursor && !lvc_utf8_is_boundary(b, send)) send--;
    suffix_len = send - cursor;

    uint8_t chunk[4096];
    size_t need = ctx_len + strlen(t->pre_tok) + prefix_len + strlen(t->suf_tok) +
                  suffix_len + strlen(t->mid_tok) + 1;
    if (need > total) {
        char *nb = realloc(out, need);
        if (!nb) { free(out); return NULL; }
        out = nb; total = need;
    }
    for (size_t pos = pstart; pos < cursor; ) {
        size_t take = cursor - pos < sizeof chunk ? cursor - pos : sizeof chunk;
        lvc_buf_read(b, pos, chunk, take);
        memcpy(out + w, chunk, take);
        w += take; pos += take;
    }
    w += (size_t)snprintf(out + w, total - w, "%s", t->suf_tok);
    for (size_t pos = cursor; pos < send; ) {
        size_t take = send - pos < sizeof chunk ? send - pos : sizeof chunk;
        lvc_buf_read(b, pos, chunk, take);
        memcpy(out + w, chunk, take);
        w += take; pos += take;
    }
    w += (size_t)snprintf(out + w, total - w, "%s", t->mid_tok);
    out[w] = 0;
    *out_len = w;
    return out;
}

int lvc_fim_log(const char *path, const char *suggestion,
                int accepted, const char *lang, const char *ctx) {
    FILE *f = fopen(path, "a");
    if (!f) {
        /* create parent dir once: path like dir/file */
        char dir[4096];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) return -1; /* no parent component */
        *slash = 0;
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
        f = fopen(path, "a");
        if (!f) return -1;
    }
    /* JSONL with escaped string */
    fputs("{\"ts\":", f);
    fprintf(f, "%lld", (long long)time(NULL));
    fputs(",\"lang\":\"", f);
    fputs(lang ? lang : "c", f);
    fputs("\",\"ctx\":\"", f);
    for (const char *p = ctx ? ctx : ""; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p == '\r') fputs("\\r", f);
        else fputc(*p, f);
    }
    fputs("\",\"suggestion\":\"", f);
    for (const char *p = suggestion; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p == '\r') fputs("\\r", f);
        else fputc(*p, f);
    }
    fprintf(f, "\",\"accepted\":%s}\n", accepted ? "true" : "false");
    int r = ferror(f) ? -1 : 0;
    fclose(f);
    return r;
}

static int ln_eq(const char *a, size_t al, const char *b, size_t bl) {
    while (al && (a[al - 1] == ' ' || a[al - 1] == '\t' ||
                  a[al - 1] == '\r' || a[al - 1] == '\n')) al--;
    while (bl && (b[bl - 1] == ' ' || b[bl - 1] == '\t' ||
                  b[bl - 1] == '\r' || b[bl - 1] == '\n')) bl--;
    return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}

static int ln_alnum(const char *s, size_t l) {
    for (size_t i = 0; i < l; i++)
        if ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= '0' && s[i] <= '9') || (s[i] & 0x80)) return 1;
    return 0;
}

int lvc_fim_sane(const char *sug, const char *ctx) {
    if (!sug || !*sug) return 0;
    size_t vis = 0;
    for (const char *p = sug; *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') vis++;
    if (vis < 3) return 0;

    /* suggestion lines */
    const char *sl[512];
    size_t sn[512], nl = 0;
    for (const char *p = sug; *p && nl < 512; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        sl[nl] = p; sn[nl] = len; nl++;
        if (!e) break;
        p = e + 1;
    }

    /* parrot check: first line identical to a recent context line */
    if (ctx && *ctx) {
        const char *cl[3];
        size_t cn[3], nc = 0;
        size_t i = strlen(ctx);
        while (i > 0 && nc < 3) {
            size_t e = i - 1, s = e;
            while (s > 0 && ctx[s - 1] != '\n') s--;
            if (ln_alnum(ctx + s, e + 1 - s) && e + 1 - s >= 4) {
                cl[nc] = ctx + s; cn[nc] = e + 1 - s; nc++;
            }
            if (s == 0) break;
            i = s - 1;
        }
        for (size_t k = 0; k < nc; k++)
            if (ln_eq(sl[0], sn[0], cl[k], cn[k])) return 0;
    }

    /* degenerate repetition: block of 1-2 lines repeated 3x consecutively */
    for (size_t m = 1; m <= 2; m++) {
        if (nl < 3 * m) continue;
        for (size_t i = 0; i + 3 * m <= nl; i++) {
            if (m == 1 && !ln_alnum(sl[i], sn[i])) continue; /* blank runs are fine */
            int rep = 1;
            for (size_t b = 1; b < 3 && rep; b++)
                for (size_t k = 0; k < m; k++)
                    if (!ln_eq(sl[i + k], sn[i + k],
                               sl[i + b * m + k], sn[i + b * m + k])) { rep = 0; break; }
            if (rep) return 0;
        }
    }
    return 1;
}

size_t lvc_fim_suffix_bound(lvc_buf *b, size_t cursor, size_t max) {
    size_t n = lvc_buf_len(b);
    size_t limit = cursor + max < n ? cursor + max : n;
    size_t i = cursor;
    int depth = 0;
    for (; i < limit; i++) {
        uint8_t c = lvc_buf_get(b, i);
        if (c == '{') depth++;
        else if (c == '}') {
            /* depth 0 closes the block that opened before the cursor */
            if (depth == 0) { i++; break; }
            depth--;
        }
    }
    if (i > n) i = n;
    return i - cursor;
}
