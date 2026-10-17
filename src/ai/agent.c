#include "agent.h"
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define READ_FILE_MAX 8192   /* read_file truncation */
#define SHELL_OUT_MAX 4096   /* run_shell output truncation */
#define SHELL_TIMEOUT_SEC 30

/* ---- growable string buffer ---- */

typedef struct { char *data; size_t len, cap; } sbuf;

static void sbuf_init(sbuf *s) { s->data = NULL; s->len = s->cap = 0; }
static void sbuf_free(sbuf *s) { free(s->data); sbuf_init(s); }
static const char *sbuf_cstr(const sbuf *s) { return s->data ? s->data : ""; }

static bool sbuf_append(sbuf *s, const char *d, size_t n) {
    if (s->len + n + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4096;
        while (ncap < s->len + n + 1) ncap *= 2;
        char *p = realloc(s->data, ncap);
        if (!p) return false;
        s->data = p;
        s->cap = ncap;
    }
    if (n) memcpy(s->data + s->len, d, n);
    s->len += n;
    s->data[s->len] = 0;
    return true;
}

static bool sapp(sbuf *s, const char *str) { return sbuf_append(s, str, strlen(str)); }

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---- printf-style error result strings for tools ---- */

static char *tool_result_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static char *tool_result_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return xstrdup(buf);
}

/* ---- minimal JSON parser / writer (no vendored JSON lib) ---- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype t;
    bool b;
    double num;
    char *str;                        /* J_STR */
    struct jval **items; size_t nitems;             /* J_ARR */
    char **keys; struct jval **vals; size_t nkeys;  /* J_OBJ */
} jval;

typedef struct { const char *p, *end; bool err; } jparser;

static jval *jnew(jtype t) {
    jval *v = calloc(1, sizeof *v);
    if (v) v->t = t;
    return v;
}

void jval_free(jval *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->nitems; i++) jval_free(v->items[i]);
    free(v->items);
    for (size_t i = 0; i < v->nkeys; i++) {
        free(v->keys[i]);
        jval_free(v->vals[i]);
    }
    free(v->keys);
    free(v->vals);
    free(v);
}

static void jarr_push(jparser *ps, jval *arr, jval *item) {
    jval **ni = realloc(arr->items, (arr->nitems + 1) * sizeof *ni);
    if (!ni) { ps->err = true; jval_free(item); return; }
    arr->items = ni;
    arr->items[arr->nitems++] = item;
}

static void jobj_push(jparser *ps, jval *obj, char *key, jval *val) {
    char **nk = realloc(obj->keys, (obj->nkeys + 1) * sizeof *nk);
    if (!nk) goto fail;
    obj->keys = nk;
    jval **nv = realloc(obj->vals, (obj->nkeys + 1) * sizeof *nv);
    if (!nv) goto fail;
    obj->vals = nv;
    obj->keys[obj->nkeys] = key;
    obj->vals[obj->nkeys] = val;
    obj->nkeys++;
    return;
fail:
    ps->err = true;
    free(key);
    jval_free(val);
}

static const jval *jobj_get(const jval *obj, const char *key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->nkeys; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->vals[i];
    return NULL;
}

static void jskip_ws(jparser *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int jhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool jhex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = jhex(p[i]);
        if (h < 0) return false;
        v = v * 16 + (unsigned)h;
    }
    *out = v;
    return true;
}

static size_t jutf8(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* parse a JSON string literal; ps->p at opening quote; returns malloc'd text */
static char *jparse_string_raw(jparser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') { ps->err = true; return NULL; }
    ps->p++;
    sbuf sb;
    sbuf_init(&sb);
    bool ok = true;
    while (ps->p < ps->end && *ps->p != '"' && ok) {
        if (*ps->p != '\\') {
            ok = sbuf_append(&sb, ps->p, 1);
            ps->p++;
            continue;
        }
        ps->p++;
        if (ps->p >= ps->end) { ok = false; break; }
        char e = *ps->p++;
        char tmp;
        switch (e) {
        case '"': case '\\': case '/': ok = sbuf_append(&sb, &e, 1); break;
        case 'n': tmp = '\n'; ok = sbuf_append(&sb, &tmp, 1); break;
        case 't': tmp = '\t'; ok = sbuf_append(&sb, &tmp, 1); break;
        case 'r': tmp = '\r'; ok = sbuf_append(&sb, &tmp, 1); break;
        case 'b': tmp = '\b'; ok = sbuf_append(&sb, &tmp, 1); break;
        case 'f': tmp = '\f'; ok = sbuf_append(&sb, &tmp, 1); break;
        case 'u': {
            unsigned cp;
            if (ps->end - ps->p < 4 || !jhex4(ps->p, &cp)) { ok = false; break; }
            ps->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (jhex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            char u8[4];
            ok = sbuf_append(&sb, u8, jutf8(cp, u8));
            break;
        }
        default: ok = false; break;
        }
    }
    if (!ok || ps->p >= ps->end) {
        sbuf_free(&sb);
        ps->err = true;
        return NULL;
    }
    ps->p++; /* closing quote */
    if (!sb.data) return xstrdup(""); /* empty string */
    return sb.data;
}

static bool jlit(jparser *ps, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) return false;
    ps->p += n;
    return true;
}

static jval *jparse_value(jparser *ps) {
    jskip_ws(ps);
    if (ps->p >= ps->end) { ps->err = true; return NULL; }
    char c = *ps->p;
    if (c == '{') {
        ps->p++;
        jval *obj = jnew(J_OBJ);
        if (!obj) { ps->err = true; return NULL; }
        jskip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
        for (;;) {
            jskip_ws(ps);
            char *key = jparse_string_raw(ps);
            if (ps->err) { jval_free(obj); return NULL; }
            jskip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                ps->err = true;
                jval_free(obj);
                return NULL;
            }
            ps->p++;
            jval *val = jparse_value(ps);
            if (ps->err) { free(key); jval_free(obj); return NULL; }
            jobj_push(ps, obj, key, val);
            if (ps->err) { jval_free(obj); return NULL; }
            jskip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
            ps->err = true;
            jval_free(obj);
            return NULL;
        }
    }
    if (c == '[') {
        ps->p++;
        jval *arr = jnew(J_ARR);
        if (!arr) { ps->err = true; return NULL; }
        jskip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
        for (;;) {
            jval *item = jparse_value(ps);
            if (ps->err) { jval_free(arr); return NULL; }
            jarr_push(ps, arr, item);
            if (ps->err) { jval_free(arr); return NULL; }
            jskip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
            ps->err = true;
            jval_free(arr);
            return NULL;
        }
    }
    if (c == '"') {
        char *s = jparse_string_raw(ps);
        if (ps->err) return NULL;
        jval *v = jnew(J_STR);
        if (!v) { free(s); ps->err = true; return NULL; }
        v->str = s;
        return v;
    }
    if (jlit(ps, "true")) {
        jval *v = jnew(J_BOOL);
        if (v) v->b = true;
        else ps->err = true;
        return v;
    }
    if (jlit(ps, "false")) {
        jval *v = jnew(J_BOOL);
        if (!v) ps->err = true;
        return v;
    }
    if (jlit(ps, "null")) return jnew(J_NULL);
    /* number */
    char *endp = NULL;
    double d = strtod(ps->p, &endp);
    if (endp == ps->p || endp > ps->end) { ps->err = true; return NULL; }
    jval *v = jnew(J_NUM);
    if (!v) { ps->err = true; return NULL; }
    v->num = d;
    ps->p = endp;
    return v;
}

/* parse a complete NUL-terminated JSON document */
static jval *json_parse(const char *s) {
    if (!s) return NULL;
    jparser ps = { s, s + strlen(s), false };
    jval *v = jparse_value(&ps);
    if (ps.err) { jval_free(v); return NULL; }
    jskip_ws(&ps);
    if (ps.p != ps.end) { jval_free(v); return NULL; }
    return v;
}

/* append s as a quoted, escaped JSON string */
static bool json_escape(sbuf *sb, const char *s) {
    if (!sbuf_append(sb, "\"", 1)) return false;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char esc[8];
        bool ok;
        switch (*p) {
        case '"':  ok = sbuf_append(sb, "\\\"", 2); break;
        case '\\': ok = sbuf_append(sb, "\\\\", 2); break;
        case '\n': ok = sbuf_append(sb, "\\n", 2); break;
        case '\r': ok = sbuf_append(sb, "\\r", 2); break;
        case '\t': ok = sbuf_append(sb, "\\t", 2); break;
        case '\b': ok = sbuf_append(sb, "\\b", 2); break;
        case '\f': ok = sbuf_append(sb, "\\f", 2); break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                ok = sbuf_append(sb, esc, 6);
            } else {
                ok = sbuf_append(sb, (const char *)p, 1);
            }
        }
        if (!ok) return false;
    }
    return sbuf_append(sb, "\"", 1);
}

static bool jdump(sbuf *sb, const jval *v) {
    char nb[48];
    switch (v->t) {
    case J_NULL: return sapp(sb, "null");
    case J_BOOL: return sapp(sb, v->b ? "true" : "false");
    case J_NUM:
        if (v->num >= -9.0e18 && v->num <= 9.0e18 && v->num == (double)(long long)v->num)
            snprintf(nb, sizeof nb, "%lld", (long long)v->num);
        else
            snprintf(nb, sizeof nb, "%.17g", v->num);
        return sapp(sb, nb);
    case J_STR: return json_escape(sb, v->str);
    case J_ARR:
        if (!sbuf_append(sb, "[", 1)) return false;
        for (size_t i = 0; i < v->nitems; i++) {
            if (i && !sbuf_append(sb, ",", 1)) return false;
            if (!jdump(sb, v->items[i])) return false;
        }
        return sbuf_append(sb, "]", 1);
    case J_OBJ:
        if (!sbuf_append(sb, "{", 1)) return false;
        for (size_t i = 0; i < v->nkeys; i++) {
            if (i && !sbuf_append(sb, ",", 1)) return false;
            if (!json_escape(sb, v->keys[i]) || !sbuf_append(sb, ":", 1)) return false;
            if (!jdump(sb, v->vals[i])) return false;
        }
        return sbuf_append(sb, "}", 1);
    }
    return false;
}

static char *jdump_str(const jval *v) {
    sbuf sb;
    sbuf_init(&sb);
    if (!jdump(&sb, v)) { sbuf_free(&sb); return NULL; }
    if (!sb.data) return xstrdup("");
    return sb.data;
}

/* ---- tool call list ---- */

typedef struct { char *name; char *args; /* raw JSON */ } tool_call;

static bool calls_push(tool_call **arr, size_t *n, const char *name, char *args_owned) {
    tool_call *na = realloc(*arr, (*n + 1) * sizeof *na);
    if (!na) { free(args_owned); return false; }
    char *nm = xstrdup(name);
    if (!nm) { free(args_owned); return false; }
    *arr = na;
    (*arr)[*n].name = nm;
    (*arr)[*n].args = args_owned;
    (*n)++;
    return true;
}

static void calls_free(tool_call *calls, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(calls[i].name);
        free(calls[i].args);
    }
    free(calls);
}

/* Fallback tool-call extraction: ```json {"tool": "name", "args": {...}} ```
 * blocks inside assistant text (models without native tool_calls). */
static void extract_fallback_calls(const char *text, tool_call **calls, size_t *ncalls) {
    const char *p = text;
    while (calls && ncalls && (p = strstr(p, "```json")) != NULL) {
        const char *body = p + 7;
        if (*body == '\r') body++;
        if (*body == '\n') body++;
        const char *close = strstr(body, "```");
        if (!close) break;
        while (body < close &&
               (*body == ' ' || *body == '\t' || *body == '\n' || *body == '\r'))
            body++;
        const char *e = close;
        while (e > body &&
               (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
            e--;
        size_t flen = (size_t)(e - body);
        char *frag = malloc(flen + 1);
        if (frag) {
            memcpy(frag, body, flen);
            frag[flen] = 0;
            jval *v = json_parse(frag);
            free(frag);
            /* accept both {"tool","args"} and OpenAI-style {"name","arguments"} */
            const jval *tool = v ? jobj_get(v, "tool") : NULL;
            if (!tool || tool->t != J_STR) tool = v ? jobj_get(v, "name") : NULL;
            if (tool && tool->t == J_STR) {
                char *args = NULL;
                const jval *ar = jobj_get(v, "args");
                if (!ar && v) ar = jobj_get(v, "arguments");
                if (ar && ar->t == J_STR) args = xstrdup(ar->str);
                else if (ar) args = jdump_str(ar);
                else args = xstrdup("{}");
                if (args && !calls_push(calls, ncalls, tool->str, args)) break;
            }
            jval_free(v);
        }
        p = close + 3;
    }
}

/* ---- streaming chat response parsing ---- */

typedef struct {
    sbuf line;  /* partial NDJSON line */
    sbuf text;  /* assistant text accumulated this turn */
    tool_call *calls;
    size_t ncalls;
    bool done, err;
    lvc_agent_cb cb;
    void *user;
} chat_stream;

static void stream_free(chat_stream *cs) {
    sbuf_free(&cs->line);
    sbuf_free(&cs->text);
    calls_free(cs->calls, cs->ncalls);
    cs->calls = NULL;
    cs->ncalls = 0;
}

/* one complete NDJSON line from /api/chat */
static void process_line(chat_stream *cs, const char *line) {
    if (cs->err || !*line) return;
    jval *root = json_parse(line);
    if (!root) { cs->err = true; return; }
    const jval *msg = jobj_get(root, "message");
    if (msg && msg->t == J_OBJ) {
        const jval *content = jobj_get(msg, "content");
        if (content && content->t == J_STR && content->str[0]) {
            /* buffered: streamed only when the round turns out to be a
             * plain answer (fallback tool-call json is never shown) */
            if (!sbuf_append(&cs->text, content->str, strlen(content->str)))
                cs->err = true;
        }
        const jval *tcs = jobj_get(msg, "tool_calls");
        if (tcs && tcs->t == J_ARR) {
            for (size_t i = 0; i < tcs->nitems && !cs->err; i++) {
                const jval *item = tcs->items[i];
                const jval *fn = jobj_get(item, "function");
                if (!fn || fn->t != J_OBJ) continue;
                const jval *nm = jobj_get(fn, "name");
                if (!nm || nm->t != J_STR) continue;
                char *args = NULL;
                const jval *ar = jobj_get(fn, "arguments");
                /* arguments arrives as a JSON string from some models,
                 * as a JSON object from others */
                if (ar && ar->t == J_STR) args = xstrdup(ar->str);
                else if (ar) args = jdump_str(ar);
                else args = xstrdup("{}");
                if (!args || !calls_push(&cs->calls, &cs->ncalls, nm->str, args))
                    cs->err = true;
            }
        }
    }
    const jval *dv = jobj_get(root, "done");
    if (dv && dv->t == J_BOOL && dv->b) cs->done = true;
    jval_free(root);
}

static size_t chat_write(char *d, size_t sz, size_t nm, void *user) {
    chat_stream *cs = user;
    size_t total = sz * nm;
    for (size_t i = 0; i < total; i++) {
        if (d[i] == '\n') {
            process_line(cs, sbuf_cstr(&cs->line));
            cs->line.len = 0;
            if (cs->line.data) cs->line.data[0] = 0;
        } else if (!sbuf_append(&cs->line, &d[i], 1)) {
            cs->err = true;
            break;
        }
    }
    return total;
}

/* ---- tools ---- */

static bool path_under(const char *path, const char *base) {
    size_t bl = strlen(base);
    if (strncmp(path, base, bl) != 0) return false;
    return path[bl] == '/' || path[bl] == '\0';
}

/* resolve rel against workspace root base; reject anything escaping it.
 * Accepts either the lexical normalization or the physical realpath
 * (so symlinked aliases of cwd, e.g. /tmp vs /private/tmp, still work). */
static bool resolve_path(const char *base, const char *rel, char *out, size_t outsz) {
    if (!rel) rel = "";
    char cand[PATH_MAX * 2];
    int cw = rel[0] == '/'
        ? snprintf(cand, sizeof cand, "%s", rel)
        : snprintf(cand, sizeof cand, "%s/%s", base, rel);
    if (cw < 0 || (size_t)cw >= sizeof cand) return false;

    /* lexical normalization of the absolute candidate */
    char norm[PATH_MAX * 2];
    size_t nl = 0;
    const char *p = cand;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t cl = (size_t)(p - s);
        if (cl == 1 && s[0] == '.') continue;
        if (cl == 2 && s[0] == '.' && s[1] == '.') {
            while (nl > 0 && norm[nl - 1] != '/') nl--;
            if (nl > 0) nl--; /* drop the '/' before the popped component */
            continue;
        }
        if (nl + cl + 2 > sizeof norm) return false;
        norm[nl++] = '/';
        memcpy(norm + nl, s, cl);
        nl += cl;
    }
    if (nl > 0) norm[nl] = 0;
    const char *lex = nl ? norm : "/";
    if (path_under(lex, base)) {
        int w = snprintf(out, outsz, "%s", lex);
        return w >= 0 && (size_t)w < outsz;
    }
    char rp[PATH_MAX];
    if (realpath(cand, rp) && path_under(rp, base)) {
        int w = snprintf(out, outsz, "%s", rp);
        return w >= 0 && (size_t)w < outsz;
    }
    return false;
}

static char *tool_run_shell(const char *base, const char *cmd) {
    int fds[2];
    if (pipe(fds) != 0) return xstrdup("error: pipe() failed");
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return xstrdup("error: fork() failed");
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (fds[1] > STDERR_FILENO) close(fds[1]);
        if (chdir(base) != 0) _exit(126);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    (void)setpgid(pid, pid); /* avoid fork race with child's own setpgid */
    close(fds[1]);

    sbuf out;
    sbuf_init(&out);
    time_t deadline = time(NULL) + SHELL_TIMEOUT_SEC;
    bool eof = false, timed_out = false;
    while (!eof && !timed_out) {
        int wait_ms = (int)(deadline - time(NULL)) * 1000;
        if (wait_ms < 0) wait_ms = 0;
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, wait_ms);
        if (pr == 0) { timed_out = true; break; }
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char chunk[4096];
        ssize_t n = read(fds[0], chunk, sizeof chunk);
        if (n == 0) { eof = true; break; }
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!sbuf_append(&out, chunk, (size_t)n)) break;
    }
    if (!eof) { /* timeout or abnormal exit: reap the whole process group */
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
    }
    close(fds[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}

    if (out.len > SHELL_OUT_MAX) {
        out.len = SHELL_OUT_MAX;
        out.data[out.len] = 0;
    }
    sbuf res;
    sbuf_init(&res);
    if (out.data) sbuf_append(&res, out.data, out.len);
    sbuf_free(&out);
    if (timed_out) sapp(&res, "\n[run_shell: timed out after 30s]");
    if (!res.data) return xstrdup("");
    return res.data;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char *tool_list_dir(const char *full, const char *disp) {
    DIR *d = opendir(full);
    if (!d) return tool_result_error("error: cannot list '%s': %s", disp, strerror(errno));
    char **names = NULL;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t l = strlen(e->d_name);
        char *nm = malloc(l + 2);
        if (!nm) continue;
        memcpy(nm, e->d_name, l + 1);
        if (e->d_type == DT_DIR) { nm[l] = '/'; nm[l + 1] = 0; }
        char **nn = realloc(names, (n + 1) * sizeof *nn);
        if (!nn) { free(nm); continue; }
        names = nn;
        names[n++] = nm;
    }
    closedir(d);
    if (names) qsort(names, n, sizeof *names, cmp_names);
    sbuf sb;
    sbuf_init(&sb);
    for (size_t i = 0; i < n; i++) {
        if (i && !sbuf_append(&sb, "\n", 1)) break;
        if (!sapp(&sb, names[i])) break;
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    if (!sb.data) return xstrdup("");
    return sb.data;
}

static char *tool_read_file(const char *full, const char *disp) {
    FILE *f = fopen(full, "rb");
    if (!f) return tool_result_error("error: cannot open '%s': %s", disp, strerror(errno));
    char buf[READ_FILE_MAX];
    size_t n = fread(buf, 1, sizeof buf, f);
    bool truncated = fgetc(f) != EOF;
    fclose(f);
    char *res = malloc(n + 1);
    if (!res) return xstrdup("error: out of memory");
    memcpy(res, buf, n);
    res[n] = 0;
    if (truncated) {
        sbuf sb;
        sbuf_init(&sb);
        if (!sbuf_append(&sb, buf, n) || !sapp(&sb, "\n[read_file: truncated at 8192 bytes]")) {
            sbuf_free(&sb);
            free(res);
            return xstrdup("error: out of memory");
        }
        free(res);
        return sb.data;
    }
    return res;
}

static char *tool_edit_file(const char *full, const char *disp,
                            const char *old_s, const char *new_s) {
    sbuf content;
    sbuf_init(&content);
    FILE *f = fopen(full, "rb");
    if (!f) return tool_result_error("error: cannot open '%s': %s", disp, strerror(errno));
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (!sbuf_append(&content, buf, n)) {
            fclose(f);
            sbuf_free(&content);
            return xstrdup("error: out of memory");
        }
    fclose(f);
    const char *body = sbuf_cstr(&content);
    char *at = old_s[0] ? strstr(body, old_s) : NULL;
    if (!at) {
        sbuf_free(&content);
        return tool_result_error(
            "error: old_string not found in '%s' — 대상을 덮어쓰려 run_shell "
            "리다이렉션을 쓰지 마라. 먼저 read_file로 정확한 내용을 확인한 뒤 "
            "old_string을 그대로 복사해 다시 시도하라.", disp);
    }
    size_t pre = (size_t)(at - body);
    size_t old_len = strlen(old_s);
    size_t rest_len = strlen(at) - old_len;
    sbuf out;
    sbuf_init(&out);
    bool ok = sbuf_append(&out, body, pre) &&
              sbuf_append(&out, new_s, strlen(new_s)) &&
              sbuf_append(&out, at + old_len, rest_len);
    sbuf_free(&content);
    if (!ok) {
        sbuf_free(&out);
        return xstrdup("error: out of memory");
    }
    f = fopen(full, "wb");
    if (!f) {
        sbuf_free(&out);
        return tool_result_error("error: cannot write '%s': %s", disp, strerror(errno));
    }
    size_t wn = out.len ? fwrite(out.data, 1, out.len, f) : 0;
    bool werr = ferror(f) != 0 || (out.len && wn != out.len);
    fclose(f);
    sbuf_free(&out);
    if (werr) return tool_result_error("error: short write to '%s'", disp);
    return xstrdup("ok");
}

static const char *jarg_str(const jval *args, const char *key) {
    const jval *v = jobj_get(args, key);
    return (v && v->t == J_STR) ? v->str : NULL;
}

static char *tool_exec(const char *base, const char *name, const char *args_json) {
    jval *args = json_parse(args_json ? args_json : "");
    if (!args || args->t != J_OBJ) {
        jval_free(args);
        return xstrdup("error: tool arguments must be a JSON object");
    }
    char *res = NULL;
    if (strcmp(name, "run_shell") == 0) {
        const char *cmd = jarg_str(args, "cmd");
        if (!cmd) res = xstrdup("error: run_shell requires string 'cmd'");
        else res = tool_run_shell(base, cmd);
    } else if (strcmp(name, "list_dir") == 0 || strcmp(name, "read_file") == 0 ||
               strcmp(name, "edit_file") == 0) {
        const char *path = jarg_str(args, "path");
        if (!path) {
            res = tool_result_error("error: %s requires string 'path'", name);
        } else {
            char full[PATH_MAX];
            if (!resolve_path(base, path, full, sizeof full))
                res = tool_result_error("error: path escapes workspace: %s", path);
            else if (strcmp(name, "list_dir") == 0)
                res = tool_list_dir(full, path);
            else if (strcmp(name, "read_file") == 0)
                res = tool_read_file(full, path);
            else {
                const char *old_s = jarg_str(args, "old_string");
                const char *new_s = jarg_str(args, "new_string");
                if (!old_s || !new_s)
                    res = xstrdup("error: edit_file requires strings 'old_string' and 'new_string'");
                else if (!old_s[0])
                    res = xstrdup("error: edit_file old_string must not be empty");
                else
                    res = tool_edit_file(full, path, old_s, new_s);
            }
        }
    } else {
        res = tool_result_error("error: unknown tool '%s'", name);
    }
    jval_free(args);
    return res;
}

/* ---- JSON tool definitions sent with every chat request ---- */

static const char *g_tools_json =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"list_dir\","
    "\"description\":\"List entries of a directory relative to the workspace root; "
    "directories have a trailing slash\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"directory path, relative to the workspace root\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
    "\"description\":\"Read a text file relative to the workspace root "
    "(truncated at 8192 bytes)\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"file path, relative to the workspace root\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"edit_file\","
    "\"description\":\"Replace the first occurrence of old_string with new_string "
    "in a file relative to the workspace root\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"file path, relative to the workspace root\"},"
    "\"old_string\":{\"type\":\"string\",\"description\":\"exact text to replace\"},"
    "\"new_string\":{\"type\":\"string\",\"description\":\"replacement text\"}},"
    "\"required\":[\"path\",\"old_string\",\"new_string\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"run_shell\","
    "\"description\":\"Run a shell command in the workspace root; returns combined "
    "stdout+stderr (30s timeout, truncated at 4096 bytes)\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"cmd\":{\"type\":\"string\",\"description\":\"shell command\"}},"
    "\"required\":[\"cmd\"]}}}"
    "]";

/* ---- libcurl global refcount (mirrors ollama.c) ---- */

static int g_curl_refs = 0;

static int curl_ref(void) {
    if (g_curl_refs++ == 0 && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        g_curl_refs--;
        return -1;
    }
    return 0;
}

static void curl_unref(void) {
    /* no curl_global_cleanup: Apple's libcurl aborts at exit; process exit cleans up */
    g_curl_refs--;
}

static size_t sbuf_write(char *d, size_t sz, size_t nm, void *user) {
    sbuf *sb = user;
    return sbuf_append(sb, d, sz * nm) ? sz * nm : 0;
}

/* GET /api/tags — installed model names */
int lvc_agent_list_models(const char *host, char ***out_names,
                          size_t *out_count) {
    *out_names = NULL;
    *out_count = 0;
    if (!host) return -1;
    if (curl_ref() != 0) return -1;
    int rc = -1;
    CURL *c = curl_easy_init();
    sbuf body;
    sbuf_init(&body);
    if (!c) { curl_unref(); return -1; }
    char url[512];
    snprintf(url, sizeof url, "http://%s/api/tags", host);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sbuf_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    CURLcode cr = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    if (cr != CURLE_OK || http_code != 200) goto out;

    jval *root = json_parse(sbuf_cstr(&body));
    if (!root) goto out;
    const jval *models = jobj_get(root, "models");
    if (!models || models->t != J_ARR) { jval_free(root); goto out; }
    char **names = malloc((models->nitems ? models->nitems : 1) * sizeof(char *));
    if (!names) { jval_free(root); goto out; }
    size_t w = 0;
    for (size_t i = 0; i < models->nitems; i++) {
        const jval *item = models->items[i];
        const jval *nm = item ? jobj_get(item, "name") : NULL;
        if (nm && nm->t == J_STR) names[w++] = xstrdup(nm->str);
    }
    jval_free(root);
    *out_names = names;
    *out_count = w;
    rc = 0;
out:
    sbuf_free(&body);
    curl_unref();
    return rc;
}

void lvc_agent_free_models(char **names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* ---- the loop ---- */

struct lvc_agent_session { sbuf msgs; size_t nmsgs; int tools_enabled; };

lvc_agent_session *lvc_agent_session_create(void) {
    lvc_agent_session *s = malloc(sizeof *s);
    if (!s) return NULL;
    sbuf_init(&s->msgs);
    s->nmsgs = 0;
    s->tools_enabled = 1;
    return s;
}

void lvc_agent_session_set_tools(lvc_agent_session *s, int enabled) {
    if (s) s->tools_enabled = enabled ? 1 : 0;
}

void lvc_agent_session_free(lvc_agent_session *s) {
    if (!s) return;
    sbuf_free(&s->msgs);
    free(s);
}

void lvc_agent_session_clear(lvc_agent_session *s) {
    if (!s) return;
    sbuf_free(&s->msgs);
    s->nmsgs = 0;
}

size_t lvc_agent_session_len(const lvc_agent_session *s) {
    return s ? s->nmsgs : 0;
}

int lvc_agent_run_session(lvc_agent_session *sess, const char *host,
                          const char *model, const char *goal,
                          const char *cwd, lvc_agent_cb cb, void *user) {
    if (!sess || !host || !model || !goal || !cwd || !cb) return -1;
    if (curl_ref() != 0) return -1;
    int rc = -1;

    char base[PATH_MAX];
    if (!realpath(cwd, base)) {
        cb(user, LVC_AGENT_ERROR, "cwd does not exist");
        curl_unref();
        return -1;
    }

    /* seed this user turn; the transcript persists across turns */
    sbuf *msgs = &sess->msgs;
    if ((sess->nmsgs > 0 && !sapp(msgs, ",")) ||
        !sapp(msgs, "{\"role\":\"user\",\"content\":") ||
        !json_escape(msgs, goal) || !sapp(msgs, "}")) {
        cb(user, LVC_AGENT_ERROR, "out of memory");
        curl_unref();
        return -1;
    }
    sess->nmsgs++;

    char *last_sig = NULL;  /* "name args" of the previous tool round */
    char *prev_sig = NULL;  /* two rounds back: catches A-B-A oscillation */
    char *last_res = NULL;  /* its result, for repeat answers */
    int force_final = 0;    /* set on repeat: next round runs without tools */
    for (int round = 0; round < LVC_AGENT_MAX_TOOL_ROUNDS; round++) {
        sbuf body;
        sbuf_init(&body);
        bool built = sapp(&body, "{\"model\":") && json_escape(&body, model) &&
                     sapp(&body, ",\"messages\":[") &&
                     sbuf_append(&body,
                                 msgs->data ? msgs->data : "", msgs->len) &&
                     sapp(&body, "],\"stream\":true,\"tools\":") &&
                     (force_final || !sess->tools_enabled
                          ? sapp(&body, "[]")
                          : sapp(&body, g_tools_json)) &&
                     sapp(&body, ",\"options\":{\"temperature\":0.2,"
                                 "\"repeat_penalty\":1.15}}");
        if (!built) {
            sbuf_free(&body);
            cb(user, LVC_AGENT_ERROR, "out of memory");
            goto out;
        }

        chat_stream cs;
        memset(&cs, 0, sizeof cs);
        cs.cb = cb;
        cs.user = user;

        char url[512];
        snprintf(url, sizeof url, "http://%s/api/chat", host);
        CURL *c = curl_easy_init();
        if (!c) {
            sbuf_free(&body);
            cb(user, LVC_AGENT_ERROR, "curl init failed");
            goto out;
        }
        struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, sbuf_cstr(&body));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, chat_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &cs);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
        CURLcode cr = curl_easy_perform(c);
        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        sbuf_free(&body);

        if (!cs.err && cr == CURLE_OK && cs.line.len) /* trailing line without \n */
            process_line(&cs, sbuf_cstr(&cs.line));

        if (cr != CURLE_OK) {
            cb(user, LVC_AGENT_ERROR, curl_easy_strerror(cr));
            stream_free(&cs);
            goto out;
        }
        if (http_code != 200) {
            char eb[64];
            snprintf(eb, sizeof eb, "chat http status %ld", http_code);
            cb(user, LVC_AGENT_ERROR, eb);
            stream_free(&cs);
            goto out;
        }
        if (cs.err) {
            cb(user, LVC_AGENT_ERROR, "malformed chat response");
            stream_free(&cs);
            goto out;
        }
        if (!cs.done) {
            cb(user, LVC_AGENT_ERROR, "chat stream ended without done flag");
            stream_free(&cs);
            goto out;
        }

        if (force_final || !sess->tools_enabled) {
            /* no tools offered: the model must answer in plain text; if it
             * still tries a fenced call, discard it and explain instead */
            extract_fallback_calls(sbuf_cstr(&cs.text), &cs.calls, &cs.ncalls);
            if (cs.ncalls > 0) {
                calls_free(cs.calls, cs.ncalls);
                cs.calls = NULL;
                cs.ncalls = 0;
                cb(user, LVC_AGENT_TOKEN,
                   force_final
                       ? "도구 호출이 반복되어 종료했다. 위 ⎿ 결과를 확인하라."
                       : "(이 대화에서는 도구를 사용할 수 없다.)");
            } else if (cs.text.len) {
                cb(user, LVC_AGENT_TOKEN, sbuf_cstr(&cs.text));
            }
            cb(user, LVC_AGENT_DONE, "");
            stream_free(&cs);
            rc = 0;
            goto out;
        }

        if (cs.ncalls == 0) /* fenced ```json tool blocks */
            extract_fallback_calls(sbuf_cstr(&cs.text), &cs.calls, &cs.ncalls);
        if (cs.ncalls == 0) {
            /* some models emit the raw JSON object without any fence */
            const char *t = sbuf_cstr(&cs.text);
            while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
            size_t tl = strlen(t);
            if (tl > 2 && t[0] == '{' && t[tl - 1] == '}') {
                jval *v = json_parse(t);
                const jval *tool = v ? jobj_get(v, "tool") : NULL;
                if (!tool || tool->t != J_STR) tool = v ? jobj_get(v, "name") : NULL;
                if (tool && tool->t == J_STR) {
                    char *args = NULL;
                    const jval *ar = jobj_get(v, "args");
                    if (!ar && v) ar = jobj_get(v, "arguments");
                    if (ar && ar->t == J_STR) args = xstrdup(ar->str);
                    else if (ar) args = jdump_str(ar);
                    else args = xstrdup("{}");
                    if (args) calls_push(&cs.calls, &cs.ncalls, tool->str, args);
                }
                jval_free(v);
            }
        }

        if (cs.ncalls == 0) { /* final answer: emit the buffered text */
            if (cs.text.len)
                cb(user, LVC_AGENT_TOKEN, sbuf_cstr(&cs.text));
            cb(user, LVC_AGENT_DONE, "");
            stream_free(&cs);
            rc = 0;
            goto out;
        }

        /* append the assistant turn (content + tool_calls) to the transcript */
        bool ok = sapp(msgs, ",{\"role\":\"assistant\",\"content\":") &&
                  json_escape(msgs, sbuf_cstr(&cs.text)) &&
                  sapp(msgs, ",\"tool_calls\":[");
        for (size_t i = 0; ok && i < cs.ncalls; i++) {
            ok = (i == 0 || sapp(msgs, ",")) &&
                 sapp(msgs, "{\"function\":{\"name\":") &&
                 json_escape(msgs, cs.calls[i].name) &&
                 sapp(msgs, ",\"arguments\":");
            jval *av = json_parse(cs.calls[i].args);
            ok = ok && (av ? jdump(msgs, av)
                           : json_escape(msgs, cs.calls[i].args));
            jval_free(av);
            ok = ok && sapp(msgs, "}}");
        }
        ok = ok && sapp(msgs, "]}");
        sess->nmsgs++;
        if (!ok) {
            cb(user, LVC_AGENT_ERROR, "out of memory");
            stream_free(&cs);
            goto out;
        }

        /* execute each tool call, streaming call/result events */
        for (size_t i = 0; i < cs.ncalls; i++) {
            sbuf evtext;
            sbuf_init(&evtext);
            ok = sapp(&evtext, cs.calls[i].name) && sapp(&evtext, ": ") &&
                 sapp(&evtext, cs.calls[i].args ? cs.calls[i].args : "");
            if (ok) cb(user, LVC_AGENT_TOOL_CALL, sbuf_cstr(&evtext));
            sbuf_free(&evtext);
            if (!ok) {
                cb(user, LVC_AGENT_ERROR, "out of memory");
                stream_free(&cs);
                goto out;
            }

            char *res;
            size_t siglen = strlen(cs.calls[i].name) +
                            strlen(cs.calls[i].args ? cs.calls[i].args : "") + 2;
            char *sig = malloc(siglen);
            snprintf(sig, siglen, "%s %s", cs.calls[i].name,
                     cs.calls[i].args ? cs.calls[i].args : "");
            if ((last_sig && !strcmp(sig, last_sig)) ||
                (prev_sig && !strcmp(sig, prev_sig))) {
                /* text-protocol models loop: replay the previous result and
                 * run the next round WITHOUT tools so the model must answer */
                size_t rl = strlen(last_res ? last_res : "") + 160;
                res = malloc(rl);
                snprintf(res, rl,
                         "[repeat] 이 호출은 직전 라운드와 동일하다. 결과:\n%s\n"
                         "도구를 더 호출하지 말고 이 결과만으로 최종 답변을 작성하라.",
                         last_res ? last_res : "");
                force_final = 1;
                free(sig);
            } else {
                res = tool_exec(base, cs.calls[i].name, cs.calls[i].args);
                if (!res) res = xstrdup("error: out of memory");
                free(prev_sig);
                prev_sig = last_sig;
                last_sig = sig;
                free(last_res);
                last_res = xstrdup(res);
            }
            cb(user, LVC_AGENT_TOOL_RESULT, res);

            /* tool role is unsupported by some chat templates (e.g.
             * qwen2.5-coder) and yields empty replies — wrap results in a
             * user message instead, which every template renders */
            sbuf hdr;
            sbuf_init(&hdr);
            ok = sapp(&hdr, "[tool ") && sapp(&hdr, cs.calls[i].name) &&
                 sapp(&hdr, " result]\n") && sapp(&hdr, res ? res : "");
            ok = ok && sapp(msgs, ",{\"role\":\"user\",\"content\":") &&
                 json_escape(msgs, sbuf_cstr(&hdr)) &&
                 sapp(msgs, "}");
            sbuf_free(&hdr);
            sess->nmsgs++;
            free(res);
            if (!ok) {
                cb(user, LVC_AGENT_ERROR, "out of memory");
                stream_free(&cs);
                goto out;
            }
        }
        stream_free(&cs);

        /* last tool round: force a conclusion instead of the round-limit error */
        if (round == LVC_AGENT_MAX_TOOL_ROUNDS - 2 &&
            sapp(msgs, ",{\"role\":\"user\",\"content\":"
                 "\"도구 사용은 여기까지다. 모은 정보로 최종 답변만 작성하라.\"}"))
            sess->nmsgs++;
    }

    cb(user, LVC_AGENT_ERROR, "tool round limit (10) reached without a final answer");
out:
    /* on error the transcript may end mid-round; callers can
     * lvc_agent_session_clear for a clean slate */
    free(last_sig);
    free(prev_sig);
    free(last_res);
    curl_unref();
    return rc;
}

int lvc_agent_run(const char *host, const char *model, const char *goal,
                  const char *cwd, lvc_agent_cb cb, void *user) {
    lvc_agent_session *s = lvc_agent_session_create();
    if (!s) return -1;
    int r = lvc_agent_run_session(s, host, model, goal, cwd, cb, user);
    lvc_agent_session_free(s);
    return r;
}
