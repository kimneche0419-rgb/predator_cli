#include "diff.h"
#include <stdlib.h>
#include <string.h>

static char **split_lines(const char *text, size_t *count) {
    size_t cap = 16, n = 0;
    char **lines = malloc(cap * sizeof(char *));
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        if (n == cap) { cap *= 2; lines = realloc(lines, cap * sizeof(char *)); }
        lines[n] = malloc(len + 1);
        memcpy(lines[n], p, len);
        lines[n][len] = 0;
        n++;
        if (!nl) break;
        p = nl + 1;
    }
    *count = n;
    return lines;
}

/* intern lines into d->owned (owned copies) so hunk pointers stay valid */
static char *intern(lvc_diff *d, const char *line) {
    for (size_t i = 0; i < d->owned_count; i++)
        if (strcmp(d->owned[i], line) == 0) return d->owned[i];
    d->owned = realloc(d->owned, (d->owned_count + 1) * sizeof(char *));
    char *copy = malloc(strlen(line) + 1);
    strcpy(copy, line);
    d->owned[d->owned_count] = copy;
    return d->owned[d->owned_count++];
}

static void push_hunk(lvc_diff *d, size_t os, size_t ns,
                      char **del, size_t dc, char **ins, size_t ic,
                      lvc_diff *intern_map) {
    (void)intern_map;
    d->hunks = realloc(d->hunks, (d->count + 1) * sizeof(lvc_hunk));
    lvc_hunk *h = &d->hunks[d->count++];
    h->old_start = os; h->new_start = ns;
    h->del_count = dc; h->ins_count = ic;
    if (dc) {
        h->del_lines = malloc(dc * sizeof(char *));
        for (size_t i = 0; i < dc; i++) h->del_lines[i] = del[i];
    } else h->del_lines = NULL;
    if (ic) {
        h->ins_lines = malloc(ic * sizeof(char *));
        for (size_t i = 0; i < ic; i++) h->ins_lines[i] = ins[i];
    } else h->ins_lines = NULL;
}

int lvc_diff_lines(const char *old_text, const char *new_text, lvc_diff *d) {
    size_t on = 0, nn = 0;
    char **ol = split_lines(old_text, &on);
    char **nl = split_lines(new_text, &nn);
    memset(d, 0, sizeof *d);

    /* intern all lines so hunk pointers stay valid after frees */
    char **o2 = malloc(on * sizeof(char *));
    char **n2 = malloc(nn * sizeof(char *));
    for (size_t i = 0; i < on; i++) o2[i] = intern(d, ol[i]);
    for (size_t i = 0; i < nn; i++) n2[i] = intern(d, nl[i]);

    /* LCS DP with cap */
    const size_t CAP = 4u * 1024 * 1024; /* 2k x 2k lines */
    if (on * nn > CAP) {
        /* fallback: single replace hunk */
        push_hunk(d, 0, 0, o2, on, n2, nn, d);
        goto out;
    }
    {
        unsigned *dp = calloc((on + 1) * (nn + 1), sizeof(unsigned));
        if (!dp) { free(o2); free(n2); goto fail; }
        for (size_t i = on; i-- > 0; )
            for (size_t j = nn; j-- > 0; )
                dp[i * (nn + 1) + j] = strcmp(o2[i], n2[j]) == 0
                    ? dp[(i + 1) * (nn + 1) + j + 1] + 1
                    : (dp[(i + 1) * (nn + 1) + j] >= dp[i * (nn + 1) + j + 1]
                        ? dp[(i + 1) * (nn + 1) + j] : dp[i * (nn + 1) + j + 1]);
        /* walk producing grouped hunks */
        size_t i = 0, j = 0;
        while (i < on || j < nn) {
            if (i < on && j < nn && strcmp(o2[i], n2[j]) == 0) { i++; j++; continue; }
            size_t os = i, ns = j, dc = 0, ic = 0;
            while (i < on && (j >= nn || dp[(i + 1) * (nn + 1) + j] >= dp[i * (nn + 1) + j + 1]) && i < on && !(j < nn && strcmp(o2[i], n2[j]) == 0)) {
                /* consume deletions greedily but stop if equal at i,j */
                if (j < nn && strcmp(o2[i], n2[j]) == 0) break;
                i++; dc++;
            }
            while (j < nn && !(i < on && j < nn && strcmp(o2[i], n2[j]) == 0)) { j++; ic++; }
            push_hunk(d, os, ns, o2 + os, dc, n2 + ns, ic, d);
        }
        free(dp);
    }
out:
    free(o2); free(n2);
    for (size_t i = 0; i < on; i++) free(ol[i]);
    for (size_t i = 0; i < nn; i++) free(nl[i]);
    free(ol); free(nl);
    return 0;
fail:
    for (size_t i = 0; i < on; i++) free(ol[i]);
    for (size_t i = 0; i < nn; i++) free(nl[i]);
    free(ol); free(nl);
    return -1;
}

char *lvc_diff_apply(const char *old_text, const lvc_diff *d) {
    size_t on = 0;
    char **ol = split_lines(old_text, &on);
    size_t cap = strlen(old_text) + 4096, len = 0;
    char *out = malloc(cap);
    size_t oi = 0, h = 0;
    while (oi < on || h < d->count) {
        if (h < d->count && oi == d->hunks[h].old_start) {
            lvc_hunk *hk = &d->hunks[h++];
            for (size_t k = 0; k < hk->ins_count; k++) {
                size_t ll = strlen(hk->ins_lines[k]);
                while (len + ll + 2 > cap) { cap *= 2; out = realloc(out, cap); }
                memcpy(out + len, hk->ins_lines[k], ll); len += ll;
                if (hk->ins_lines[k][ll-1] != '\n') out[len++] = '\n';
            }
            oi += hk->del_count;
        } else if (oi < on) {
            size_t ll = strlen(ol[oi]);
            while (len + ll + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + len, ol[oi], ll); len += ll;
            if (ol[oi][ll-1] != '\n') out[len++] = '\n';
            oi++;
        } else break;
    }
    out[len] = 0;
    for (size_t i = 0; i < on; i++) free(ol[i]);
    free(ol);
    return out;
}

void lvc_diff_free(lvc_diff *d) {
    for (size_t i = 0; i < d->count; i++) {
        free(d->hunks[i].del_lines);
        free(d->hunks[i].ins_lines);
    }
    free(d->hunks);
    for (size_t i = 0; i < d->owned_count; i++) free(d->owned[i]);
    free(d->owned);
    memset(d, 0, sizeof *d);
}
