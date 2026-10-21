/* lvcli — Claude Code-style terminal coding agent on local Ollama.
 *
 * One-shot:  lvcli [-m model] [-C dir] [-H host:port] "task"
 * REPL:      lvcli [-m model] [-C dir] [-H host:port]
 * REPL commands: /help /model [name] /cwd dir /reset /stat /quit
 *
 * Multi-turn memory lives in an lvc_agent_session; tools (list_dir,
 * read_file, edit_file, run_shell) are sandboxed to the working directory.
 */
#include "../ai/agent.h"
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* omp (oh-my-pi) system-prompt.md style: terse RFC2119 rule sections.
 * Appended at runtime: § Workspace, § Project instructions (CLAUDE.md),
 * § Skills catalog. */
static const char *SYSTEM_PROMPT =
    "§ Role\n"
    "You are predator_cli, a terminal coding agent on the user's macOS machine.\n"
    "Explanations: concise Korean. Code: clean, minimal, no dead weight.\n"
    "§ Tools\n"
    "list_dir/read_file/edit_file/run_shell/read_skill are sandboxed to the "
    "workspace.\n"
    "MUST use edit_file for file edits. NEVER overwrite whole files via run_shell "
    "redirection. MUST read a file before editing it.\n"
    "§ Conversation\n"
    "Greetings and general questions: answer directly, no tools.\n"
    "§ Report\n"
    "State what you changed and how you verified it. Ground claims in tool "
    "results; never fabricate output.";

/* best all-round chat default among small local models; /model marks it */
static const char *DEFAULT_MODEL = "qwen2.5-coder:3b";

static void on_sigint(int sig) { (void)sig; _exit(130); }

static char g_model_buf[256]; /* /model selection storage */
static char g_cwd_buf[1024];  /* /cwd selection storage */
static char g_mem_src[1024] = ""; /* project memory source path */

/* ---- project memory (Claude Code CLAUDE.md convention) ---- */

static char *read_file_capped(const char *path, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *out = malloc(cap + 1);
    if (!out) { fclose(f); return NULL; }
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    out[n] = 0;
    return out;
}

/* nearest AGENTS.md/CLAUDE.md walking up from cwd, plus ~/.claude/CLAUDE.md.
 * returns malloc'd combined text or NULL; source path in g_mem_src. */
static char *find_project_memory(const char *cwd) {
    char cur[4096];
    if (!realpath(cwd, cur)) snprintf(cur, sizeof cur, "%s", cwd);
    char out[12288];
    size_t w = 0;
    out[0] = 0;
    g_mem_src[0] = 0;
    for (int depth = 0; depth < 8; depth++) {
        for (int t = 0; t < 2; t++) {
            char path[4200];
            snprintf(path, sizeof path, "%s/%s", cur,
                     t == 0 ? "AGENTS.md" : "CLAUDE.md");
            char *body = read_file_capped(path, 6144);
            if (body) {
                w += (size_t)snprintf(out + w, sizeof out - w,
                                      "[%s]\n%s\n", path, body);
                if (!g_mem_src[0])
                    snprintf(g_mem_src, sizeof g_mem_src, "%s", path);
                free(body);
            }
        }
        if (w > 8000) break;
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = 0;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        char path[4200];
        snprintf(path, sizeof path, "%s/.claude/CLAUDE.md", home);
        char *body = read_file_capped(path, 4096);
        if (body) {
            w += (size_t)snprintf(out + w, sizeof out - w,
                                  "[~/.claude/CLAUDE.md]\n%s\n", body);
            snprintf(g_mem_src, sizeof g_mem_src, "%s", path);
            free(body);
        }
    }
    return w ? strdup(out) : NULL;
}

/* ---- omp-style skills: scan .predator/skills dirs, build the catalog ---- */

static void scan_skills(const char *cwd, lvc_agent_session *sess,
                        char *cat, size_t catsz) {
    const char *home = getenv("HOME");
    char dirs[4][4096];
    int dn = 0;
    if (cwd && *cwd) {
        snprintf(dirs[dn], sizeof dirs[dn], "%s/.predator/skills", cwd);
        if (access(dirs[dn], R_OK) == 0) dn++;
    }
    if (home && *home && dn < 4) {
        snprintf(dirs[dn], sizeof dirs[dn], "%s/.predator/skills", home);
        if (access(dirs[dn], R_OK) == 0 &&
            (dn == 0 || strcmp(dirs[dn], dirs[0]) != 0))
            dn++;
    }
    cat[0] = 0;
    size_t cw = 0;
    for (int i = 0; i < dn; i++) {
        lvc_agent_session_add_skills_dir(sess, dirs[i]);
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char cpath[4200], content_path[4200];
            snprintf(cpath, sizeof cpath, "%s/%s", dirs[i], e->d_name);
            struct stat st;
            if (stat(cpath, &st) == 0 && S_ISDIR(st.st_mode))
                snprintf(content_path, sizeof content_path,
                         "%s/SKILL.md", cpath);
            else
                snprintf(content_path, sizeof content_path, "%s", cpath);
            char name[256];
            snprintf(name, sizeof name, "%s", e->d_name);
            char *md = strstr(name, ".md");
            if (md && !md[3]) *md = 0;
            char desc[160] = "(no description)";
            FILE *f = fopen(content_path, "r");
            if (!f) continue;
            char l[256];
            while (fgets(l, sizeof l, f)) {
                char *p = l;
                while (*p == ' ' || *p == '#') p++;
                if (*p && *p != '\n') {
                    size_t dl = strlen(p);
                    while (dl && (p[dl - 1] == '\n' || p[dl - 1] == '\r'))
                        p[--dl] = 0;
                    if (dl > 140) dl = 140;
                    snprintf(desc, sizeof desc, "%.*s", (int)dl, p);
                    break;
                }
            }
            fclose(f);
            if (cw + strlen(name) + strlen(desc) + 8 < catsz)
                cw += (size_t)snprintf(cat + cw, catsz - cw,
                                       "- %s: %s\n", name, desc);
        }
        closedir(d);
    }
}

/* omp system-prompt.md structure: base rules + § Workspace + § Project
 * instructions (CLAUDE.md) + § Skills catalog */
static void rebuild_system(const char *cwd, lvc_agent_session *sess) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    if (!f) return;
    fputs(SYSTEM_PROMPT, f);
    fprintf(f, "\n§ Workspace\n%s\n", cwd);
    char *mem = find_project_memory(cwd);
    if (mem)
        fprintf(f,
            "\n§ Project instructions (CLAUDE.md/AGENTS.md — MUST follow)\n%s\n",
            mem);
    char cat[8192];
    scan_skills(cwd, sess, cat, sizeof cat);
    if (*cat)
        fprintf(f,
            "\n§ Skills\nMatching skill → MUST call read_skill before using it.\n%s",
            cat);
    fclose(f);
    lvc_agent_session_set_system(sess, buf);
    free(buf);
    free(mem);
}

/* ---- raw-mode line editor + arrow-key picker (no external deps) ---- */

static int ui_color = 0;          /* ANSI enabled */
static int ui_truecolor = 0;      /* COLORTERM=truecolor */

#define C_RESET   (ui_color ? "\033[0m" : "")
#define C_ACCENT  (ui_color ? (ui_truecolor ? "\033[38;2;217;119;87m" \
                                            : "\033[38;5;209m") : "")
#define C_DIM     (ui_color ? "\033[2m" : "")
#define C_BOLD    (ui_color ? "\033[1m" : "")
#define C_CODE    (ui_color ? "\033[36m" : "")
#define C_ERR     (ui_color ? "\033[31m" : "")

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#include <termios.h>
#include <unistd.h>

enum { K_CHAR, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_BACKSPACE,
       K_DELETE, K_HOME, K_END, K_EOF, K_CTRL_C, K_NONE };

static struct termios g_orig_tio;

static void pred_raw(int on) {
    static int saved = 0;
    struct termios t;
    if (on) {
        tcgetattr(0, &g_orig_tio);
        t = g_orig_tio;
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        saved = 1;
    } else if (saved) {
        tcsetattr(0, TCSANOW, &g_orig_tio);
        saved = 0;
    }
}

/* decode one utf-8 char; returns byte length, sets *w to display width */
static size_t utf8_width_step(const char *s, size_t len, int *w) {
    unsigned char c = (unsigned char)(len ? s[0] : 0);
    size_t n = 1;
    uint32_t cp = c;
    if (c >= 0xC0) {
        if ((c & 0xF8) == 0xF0) n = 4;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xE0) == 0xC0) n = 2;
        cp = c & (unsigned)(0xFF >> (n + 1));
        for (size_t k = 1; k < n && k < len; k++)
            cp = (cp << 6) | ((unsigned char)s[k] & 0x3F);
    }
    if (w) {
        int wide = (cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 ||
                   cp == 0x232A || (cp >= 0x2E80 && cp <= 0xA4CF &&
                                    cp != 0x303F) ||
                   (cp >= 0xAC00 && cp <= 0xD7A3) ||
                   (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0xFE30 && cp <= 0xFE6F) ||
                   (cp >= 0xFF00 && cp <= 0xFF60) ||
                   (cp >= 0xFFE0 && cp <= 0xFFE6);
        *w = cp == 0 ? 0 : (wide ? 2 : 1);
    }
    return len ? n : 0;
}

static size_t line_width(const char *s, size_t len) {
    size_t w = 0, i = 0;
    int cw;
    while (i < len) {
        i += utf8_width_step(s + i, len - i, &cw);
        w += (size_t)cw;
    }
    return w;
}

/* move cursor left by one display char from byte offset cur */
static size_t prev_boundary(const char *s, size_t cur) {
    if (cur == 0) return 0;
    size_t p = cur - 1;
    while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) p--;
    return p;
}
static size_t next_boundary(const char *s, size_t len, size_t cur) {
    if (cur >= len) return len;
    size_t p = cur + 1;
    while (p < len && ((unsigned char)s[p] & 0xC0) == 0x80) p++;
    return p;
}

/* read one key in raw mode; chout receives printable char when K_CHAR */
static int read_key(char *chout) {
    char c;
    ssize_t n = read(0, &c, 1);
    if (n <= 0) return K_EOF;
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 127 || c == 8) return K_BACKSPACE;
    if (c == 3) return K_CTRL_C;
    if (c == 4) return K_EOF;
    if (c == 1) return K_HOME;
    if (c == 5) return K_END;
    if (c == 27) {
        /* try to read the sequence; bare esc (timeout) = K_ESC */
        struct termios t;
        tcgetattr(0, &t);
        struct termios t2 = t;
        t2.c_cc[VMIN] = 0;
        t2.c_cc[VTIME] = 1; /* 100 ms */
        tcsetattr(0, TCSANOW, &t2);
        char c2;
        n = read(0, &c2, 1);
        if (n <= 0) { tcsetattr(0, TCSANOW, &t); *chout = 27; return K_CHAR; }
        if (c2 == '[' || c2 == 'O') {
            char c3;
            n = read(0, &c3, 1);
            tcsetattr(0, TCSANOW, &t);
            if (n <= 0) return K_ESC;
            switch (c3) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            default: return K_NONE;
            }
        }
        tcsetattr(0, TCSANOW, &t);
        *chout = c2;
        return K_CHAR;
    }
    *chout = c;
    return K_CHAR;
}

typedef struct {
    char **items;
    size_t n, cap;
    size_t pos; /* current history cursor: n == fresh input */
} pred_hist;

static void hist_push(pred_hist *h, const char *s) {
    if (!s || !*s) return;
    if (h->n && !strcmp(h->items[h->n - 1], s)) { h->pos = h->n; return; }
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 16;
        h->items = realloc(h->items, h->cap * sizeof(char *));
    }
    h->items[h->n++] = strdup(s);
    h->pos = h->n;
}

/* raw-mode editable line: history nav, utf-8 aware. returns malloc'd line
 * ("" on ctrl-c), NULL on EOF. */
static char *pred_readline(const char *prompt, pred_hist *h) {
    if (!isatty(0)) { /* piped: plain getline */
        char *line = NULL;
        size_t cap = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) { free(line); return NULL; }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        return line;
    }

    char buf[8192];
    buf[0] = 0; /* uninitialized stack memory must never reach %s renders */
    size_t len = 0, cur = 0;
    size_t hpos = h->n;
    char draft[8192] = "";
    pred_raw(1);

#define RL_RENDER()                                                    \
    do {                                                               \
        fprintf(stderr, "\r\033[K%s%s", prompt, buf);                  \
        size_t tw = line_width(buf + cur, len - cur);                  \
        if (tw) fprintf(stderr, "\033[%zuD", tw);                      \
        fflush(stderr);                                                \
    } while (0)

    RL_RENDER();
    for (;;) {
        char ch;
        int k = read_key(&ch);
        if (k == K_EOF) {
            pred_raw(0);
            fprintf(stderr, "\r\033[K\n");
            return NULL;
        }
        if (k == K_ENTER) {
            pred_raw(0);
            fprintf(stderr, "\n"); /* leave the submitted line intact */
            break;
        }
        if (k == K_CTRL_C) {
            buf[0] = 0;
            pred_raw(0);
            fprintf(stderr, "\r\033[K\n");
            return strdup("");
        }
        switch (k) {
        case K_BACKSPACE:
            if (cur > 0) {
                size_t p = prev_boundary(buf, cur);
                memmove(buf + p, buf + cur, len - cur);
                len -= cur - p;
                cur = p;
                buf[len] = 0;
                RL_RENDER();
            }
            break;
        case K_DELETE: {
            size_t nx = next_boundary(buf, len, cur);
            if (nx > cur) {
                memmove(buf + cur, buf + nx, len - nx);
                len -= nx - cur;
                buf[len] = 0;
                RL_RENDER();
            }
            break;
        }
        case K_LEFT:
            if (cur > 0) {
                cur = prev_boundary(buf, cur);
                RL_RENDER();
            }
            break;
        case K_RIGHT:
            if (cur < len) {
                cur = next_boundary(buf, len, cur);
                RL_RENDER();
            }
            break;
        case K_HOME:
            cur = 0;
            RL_RENDER();
            break;
        case K_END:
            cur = len;
            RL_RENDER();
            break;
        case K_UP:
            if (h->n == 0) break;
            if (hpos == h->n) { /* entering history: stash draft */
                snprintf(draft, sizeof draft, "%s", buf);
                hpos = h->n - 1;
            } else if (hpos > 0)
                hpos--;
            snprintf(buf, sizeof buf, "%s", h->items[hpos]);
            len = cur = strlen(buf);
            RL_RENDER();
            break;
        case K_DOWN:
            if (hpos < h->n) {
                hpos++;
                if (hpos == h->n)
                    snprintf(buf, sizeof buf, "%s", draft);
                else
                    snprintf(buf, sizeof buf, "%s", h->items[hpos]);
                len = cur = strlen(buf);
                RL_RENDER();
            }
            break;
        case K_CHAR: {
            if (ch == '\n' || ch == '\r') break;
            if (len + 1 >= sizeof buf) break;
            if (cur == len) {
                buf[len++] = ch;
                cur = len;
            } else {
                memmove(buf + cur + 1, buf + cur, len - cur);
                buf[cur++] = ch;
                len++;
            }
            buf[len] = 0; /* keep NUL-terminated: %s renders stop here */
            RL_RENDER(); /* multibyte chars redraw per byte; correct on the
                            final byte */
            break;
        }
        default:
            break;
        }
    }
#undef RL_RENDER
    pred_raw(0);
    buf[len] = 0;
    return strdup(buf);
}

/* arrow-key picker: returns malloc'd selection, NULL on cancel */
static char *pred_picker(char **items, size_t n, const char *title) {
    if (!isatty(0) || !ui_color || n == 0) return NULL;
    size_t idx = 0;
    size_t printed = 0; /* total rendered lines (title + items) */
    pred_raw(1);
    for (;;) {
        if (printed) fprintf(stderr, "\033[%zuA\r", printed);
        fprintf(stderr, "\033[K%s%s%s\n", C_ACCENT, title, C_RESET);
        for (size_t i = 0; i < n; i++)
            fprintf(stderr, "\033[K%s%s%s %s%s\n",
                    i == idx ? C_ACCENT : C_DIM,
                    i == idx ? "❯ " : "  ", C_RESET, items[i],
                    i == idx ? "  ←" : "");
        printed = n + 1;
        fflush(stderr);
        char ch;
        int k = read_key(&ch);
        if (k == K_UP) {
            if (idx > 0) idx--;
        } else if (k == K_DOWN) {
            if (idx + 1 < n) idx++;
        } else if (k == K_ENTER) {
            pred_raw(0);
            return strdup(items[idx]);
        } else if (k == K_ESC || k == K_CTRL_C ||
                   (k == K_CHAR && ch == 'q')) {
            pred_raw(0);
            fprintf(stderr, "\033[%zuA\r\033[K%s(cancelled)%s\n", printed,
                    C_DIM, C_RESET);
            return NULL;
        }
    }
}

/* ---- Claude Code-style UI ---- */

/* render one assistant line with light markdown styling */
static int g_in_fence = 0;

static void emit_inline(const char *s) {
    /* `code` spans and **bold** spans, per line */
    for (const char *p = s; *p; ) {
        if (p[0] == '`') {
            const char *e = strchr(p + 1, '`');
            if (e) {
                printf("%s%.*s%s", C_CODE, (int)(e - p - 1), p + 1, C_RESET);
                p = e + 1;
                continue;
            }
        }
        if (p[0] == '*' && p[1] == '*') {
            const char *e = strstr(p + 2, "**");
            if (e) {
                printf("%s%.*s%s", C_BOLD, (int)(e - p - 2), p + 2, C_RESET);
                p = e + 2;
                continue;
            }
        }
        fputc(*p, stdout);
        p++;
    }
}

static void md_line(const char *line) {
    /* the model often echoes its own tool call as raw json — hide it */
    if (!g_in_fence && line[0] == '{' && line[strlen(line) - 1] == '}' &&
        (strstr(line, "\"name\"") || strstr(line, "\"tool\"")))
        return;
    if (!strncmp(line, "```", 3)) {
        if (!g_in_fence) {
            g_in_fence = 1;
            if (ui_color) printf("%s╭─╮%s\n", C_ACCENT, C_RESET);
        }
        return;
    }
    if (g_in_fence) {
        if (ui_color) printf("%s│%s ", C_ACCENT, C_RESET);
        printf("%s\n", line);
        return;
    }
    if (line[0] == '#' && ui_color) {
        printf("%s%s%s%s\n", C_BOLD, C_ACCENT, line, C_RESET);
        return;
    }
    if ((!strncmp(line, "- ", 2) || !strncmp(line, "* ", 2)) && ui_color) {
        printf("%s•%s ", C_ACCENT, C_RESET);
        emit_inline(line + 2);
        printf("\n");
        return;
    }
    emit_inline(line);
    printf("\n");
}

/* pending partial line for streamed tokens */
static char pbuf[8192];
static size_t plen = 0;

static void stream_flush(void) {
    pbuf[plen] = 0;
    md_line(pbuf);
    plen = 0;
}

static void stream_text(const char *s) {
    for (const char *p = s; *p; p++) {
        if (plen < sizeof pbuf - 1) pbuf[plen++] = *p;
        if (*p == '\n') stream_flush();
    }
}

/* braille spinner while the model thinks */
static volatile int spin_run = 0;
static pthread_t spin_tid;

static void *spin_fn(void *arg) {
    const char *frames[] = {"✻", "✽", "✶", "✳"};
    const char *verbs[] = {"Thinking", "Pondering", "Working", "Conjuring"};
    int f = 0, v = (int)(time(NULL) % 4);
    time_t t0 = time(NULL);
    (void)arg;
    while (spin_run) {
        if (ui_color)
            fprintf(stderr, "\r\033[K%s%s %s… (%lds)%s", C_ACCENT,
                    frames[f % 4], verbs[v % 4],
                    (long)(time(NULL) - t0), C_RESET);
        else
            fprintf(stderr, "\r\033[K* %s… (%lds)", verbs[v % 4],
                    (long)(time(NULL) - t0));
        fflush(stderr);
        f++;
        if (f % 4 == 0) v++;
        sleep_ms(90);
    }
    fprintf(stderr, "\r\033[K");
    fflush(stderr);
    return NULL;
}

static void spin_start(void) {
    if (!spin_run) {
        spin_run = 1;
        pthread_create(&spin_tid, NULL, spin_fn, NULL);
    }
}

static void spin_stop(void) {
    if (spin_run) {
        spin_run = 0;
        pthread_join(spin_tid, NULL);
    }
}

/* tool display: Claude Code-style "● Read(path)" + "⎿ result" */
static const char *pretty_tool(const char *name) {
    if (!strcmp(name, "list_dir")) return "List";
    if (!strcmp(name, "read_file")) return "Read";
    if (!strcmp(name, "edit_file")) return "Edit";
    if (!strcmp(name, "run_shell")) return "Bash";
    return name;
}

/* primary argument of a tool call: "path"/"cmd" value, or the raw json */
static void tool_key(const char *json, char *out, size_t outn) {
    const char *keys[] = {"\"path\":\"", "\"path\": \"", "\"cmd\":\"", "\"cmd\": \""};
    for (size_t i = 0; i < 4; i++) {
        const char *p = strstr(json ? json : "", keys[i]);
        if (!p) continue;
        p += strlen(keys[i]);
        size_t w = 0;
        while (*p && w < outn - 1) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') { out[w++] = ' '; p++; continue; }
            }
            if (*p == '"') break;
            out[w++] = *p++;
        }
        out[w] = 0;
        return;
    }
    snprintf(out, outn, "%s", json ? json : "");
}

static void on_event(void *user, lvc_agent_event ev, const char *text) {
    (void)user;
    spin_stop();
    switch (ev) {
    case LVC_AGENT_TOKEN:
        stream_text(text ? text : "");
        break;
    case LVC_AGENT_TOOL_CALL: {
        const char *sp = text ? strchr(text, ' ') : NULL;
        size_t nlen = sp ? (size_t)(sp - text) : strlen(text ? text : "");
        char name[32];
        snprintf(name, sizeof name, "%.*s", (int)nlen, text ? text : "");
        size_t nl2 = strlen(name);
        if (nl2 && name[nl2 - 1] == ':') name[nl2 - 1] = 0;
        char key[128];
        tool_key(sp ? sp + 1 : "", key, sizeof key);
        printf("%s●%s %s%s%s(%s%s%s)%s\n",
               C_ACCENT, C_RESET, C_BOLD, pretty_tool(name), C_RESET,
               C_DIM, key, C_RESET, C_RESET);
        break;
    }
    case LVC_AGENT_TOOL_RESULT: {
        const char *nl = text ? strchr(text, '\n') : NULL;
        size_t first = nl ? (size_t)(nl - text) : (text ? strlen(text) : 0);
        if (first > 100) first = 100;
        size_t lines = 1;
        for (const char *p = text; p && *p; p++) if (*p == '\n') lines++;
        printf("%s  ⎿ %.*s%s", C_DIM, (int)first, text ? text : "", C_RESET);
        if (lines > 1) printf(" %s(+%zu lines)%s", C_DIM, lines - 1, C_RESET);
        printf("\n");
        spin_start(); /* next round: the model is thinking again */
        break;
    }
    case LVC_AGENT_DONE:
        stream_flush();
        printf("\n");
        break;
    case LVC_AGENT_ERROR:
        stream_flush();
        printf("%s[error] %s%s\n", C_ERR, text ? text : "", C_RESET);
        break;
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-m model] [-C dir] [-H host:port] [-r session.jsonl] [prompt]\n"
            "  prompt가 있으면 원샷 실행, 없으면 대화형 REPL\n"
            "  REPL 명령: /help /model [name|#] /cwd dir /reset /stat /tools /resume [file] /quit\n",
            prog);
}

/* greetings and short chat don't need the tool loop */
static int casual_input(const char *s) {
    if (strlen(s) > 12) return 0;
    return strpbrk(s, "./\\`0123456789") == NULL;
}

static void banner(const char *model, const char *host, const char *cwd) {
    if (!ui_color) {
        fprintf(stderr, "predator_cli — model %s @ %s, cwd %s (/help)\n",
                model, host, cwd);
        return;
    }
    char l1[256], l2[512];
    snprintf(l1, sizeof l1, "✻ predator_cli");
    snprintf(l2, sizeof l2, "%s · %s", model, cwd);
    size_t w1 = line_width(l1, strlen(l1));
    size_t w2 = line_width(l2, strlen(l2));
    size_t w = (w1 > w2 ? w1 : w2);
    if (w > 60) w = 60;
    char edge[256];
    size_t ew = 0;
    while (ew / 3 < w + 4 && ew + 4 < sizeof edge) {
        memcpy(edge + ew, "─", 3);
        ew += 3;
    }
    edge[ew] = 0;
    char pad1[96], pad2[96];
    size_t p1 = w + 2 > w1 ? w + 2 - w1 : 0;
    size_t p2 = w + 2 > w2 ? w + 2 - w2 : 0;
    if (p1 > 90) p1 = 90;
    if (p2 > 90) p2 = 90;
    memset(pad1, ' ', p1);
    pad1[p1] = 0;
    memset(pad2, ' ', p2);
    pad2[p2] = 0;
    fprintf(stderr,
            "%s╭%s╮%s\n"
            "%s│ %s%s%s│%s\n"
            "%s│ %s%s%s│%s\n"
            "%s╰%s╯%s\n"
            "  %s/help%s for commands · ctrl-c to exit\n"
            "\n",
            C_ACCENT, edge, C_RESET,
            C_ACCENT, C_BOLD, l1, pad1, C_RESET,
            C_ACCENT, C_DIM, l2, pad2, C_RESET,
            C_ACCENT, edge, C_RESET,
            C_DIM, C_RESET);
}

int main(int argc, char **argv) {
    const char *model = DEFAULT_MODEL;
    const char *env_model = getenv("PREDATOR_MODEL");
    if (env_model && *env_model) model = env_model;
    const char *cwd = ".";
    const char *host = "127.0.0.1:11434";
    const char *oneshot = NULL;
    const char *resume_path = NULL;
    ui_color = isatty(1) && isatty(2);
    {
        const char *ct = getenv("COLORTERM");
        ui_truecolor = ui_color && ct && strstr(ct, "truecolor") != NULL;
    }
    int opt;
    while ((opt = getopt(argc, argv, "m:C:H:r:h")) != -1) {
        switch (opt) {
        case 'm': model = optarg; break;
        case 'C': cwd = optarg; break;
        case 'H': host = optarg; break;
        case 'r': resume_path = optarg; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind < argc) oneshot = argv[optind];
    signal(SIGINT, on_sigint);

    lvc_agent_session *sess = lvc_agent_session_create();
    if (!sess) return 1;
    rebuild_system(cwd, sess);
    int rc = 0;

    if (resume_path) {
        if (lvc_agent_session_load(sess, resume_path) == 0)
            fprintf(stderr, "[resumed %zu messages from %s]\n",
                    lvc_agent_session_len(sess), resume_path);
        else
            fprintf(stderr, "[resume failed: %s]\n", resume_path);
    }

    if (oneshot) {
        char *goal = strdup(oneshot);
        if (!goal) return 1;
        lvc_agent_session_set_tools(sess, !casual_input(oneshot));
        spin_start();
        rc = lvc_agent_run_session(sess, host, model, goal, cwd,
                                   on_event, NULL) == 0 ? 0 : 1;
        free(goal);
        lvc_agent_session_free(sess);
        return rc;
    }

    banner(model, host, cwd);
    pred_hist hist = {0};
    int tty_in = isatty(0);
    for (;;) {
        char *line;
        if (tty_in)
            line = pred_readline("❯ ", &hist); /* raw mode: history nav */
        else {
            line = NULL;
            size_t cap = 0;
            ssize_t n = getline(&line, &cap, stdin);
            if (n < 0) { free(line); break; }
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = 0;
        }
        if (!line) break; /* EOF: quit */
        if (!*line) { free(line); continue; } /* empty / ctrl-c */
        if (line[0] == '/') {
            if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) {
                printf("%s✻ session ended%s\n", C_DIM, C_RESET);
                break;
            }
            else if (!strcmp(line, "/help"))
                fprintf(stderr,
                    "/model [name|#] /cwd dir /reset /stat /tools /skills /memory /reload /resume [file] /quit\n");
            else if (!strcmp(line, "/reset")) {
                lvc_agent_session_clear(sess);
                rebuild_system(cwd, sess);
                fprintf(stderr, "[session cleared]\n");
            } else if (!strcmp(line, "/stat")) {
                fprintf(stderr, "[messages: %zu]\n",
                        lvc_agent_session_len(sess));
            } else if (!strcmp(line, "/model") || !strncmp(line, "/model ", 7)) {
                const char *arg = line[7] ? line + 7 : NULL;
                if (!arg) { /* interactive picker (tty) or plain list */
                    char **names = NULL;
                    size_t cnt = 0;
                    if (lvc_agent_list_models(host, &names, &cnt) == 0 && cnt) {
                        if (isatty(0)) {
                            char title[512];
                            snprintf(title, sizeof title,
                                     "model — ↑/↓ 이동 · Enter 선택 · Esc 취소 · "
                                     "대화 권장: %s", DEFAULT_MODEL);
                            char *sel = pred_picker(names, cnt, title);
                            if (sel) {
                                snprintf(g_model_buf, sizeof g_model_buf, "%s", sel);
                                model = g_model_buf;
                                fprintf(stderr, "[model: %s]\n", model);
                                free(sel);
                            }
                        } else {
                            for (size_t i = 0; i < cnt; i++)
                                fprintf(stderr, "  %zu. %s\n", i + 1, names[i]);
                        }
                    } else {
                        fprintf(stderr,
                            "[cannot list models — is Ollama running at %s?]\n",
                            host);
                    }
                } else {
                    char *end = NULL;
                    long num = strtol(arg, &end, 10);
                    if (end && *end == 0 && num > 0) {
                        char **names = NULL;
                        size_t cnt = 0;
                        if (lvc_agent_list_models(host, &names, &cnt) == 0 &&
                            num <= (long)cnt) {
                            snprintf(g_model_buf, sizeof g_model_buf, "%s",
                                     names[num - 1]);
                            model = g_model_buf;
                        } else
                            fprintf(stderr, "[no model #%ld]\n", num);
                        if (names) lvc_agent_free_models(names, cnt);
                    } else {
                        snprintf(g_model_buf, sizeof g_model_buf, "%s", arg);
                        model = g_model_buf;
                    }
                    fprintf(stderr, "[model: %s]\n", model);
                }
            } else if (!strcmp(line, "/tools")) {
                fprintf(stderr,
                    "tools (workspace-sandboxed):\n"
                    "  list_dir(path)            list a directory\n"
                    "  read_file(path)           read a file (8 KB cap)\n"
                    "  edit_file(path,old,new)   replace old_string with new_string\n"
                    "  run_shell(cmd)            shell command (30 s timeout)\n"
                    "  read_skill(name)          load an installed skill (/skills)\n");
            } else if (!strcmp(line, "/skills")) {
                char cat[8192];
                scan_skills(cwd, sess, cat, sizeof cat);
                if (*cat)
                    fprintf(stderr, "skills:\n%s", cat);
                else
                    fprintf(stderr,
                        "no skills — put *.md in .predator/skills/ (project) or "
                        "~/.predator/skills/\n");
            } else if (!strcmp(line, "/memory")) {
                char *mem = find_project_memory(cwd);
                if (mem) {
                    fprintf(stderr, "project memory [%s]:\n%.*s\n",
                            g_mem_src, 600, mem);
                    free(mem);
                } else
                    fprintf(stderr,
                        "no project memory — CLAUDE.md/AGENTS.md를 작업 디렉터리에 "
                        "만들면 매 세션 자동 주입된다\n");
            } else if (!strcmp(line, "/reload")) {
                rebuild_system(cwd, sess);
                fprintf(stderr, "[reloaded: memory %s, skills scanned]\n",
                        g_mem_src[0] ? g_mem_src : "(none)");
            } else if (!strncmp(line, "/resume", 7) &&
                       (line[7] == 0 || line[7] == ' ')) {
                const char *p = line[7] ? line + 8 : NULL;
                char defbuf[512];
                if (!p || !*p) {
                    const char *home = getenv("HOME");
                    if (!home) continue;
                    snprintf(defbuf, sizeof defbuf,
                             "%s/.predator/last_session.jsonl", home);
                    p = defbuf;
                }
                if (lvc_agent_session_load(sess, p) == 0)
                    fprintf(stderr, "[resumed %zu messages from %s]\n",
                            lvc_agent_session_len(sess), p);
                else
                    fprintf(stderr, "[resume failed: %s]\n", p);
            } else if (!strncmp(line, "/cwd ", 5)) {
                snprintf(g_cwd_buf, sizeof g_cwd_buf, "%s", line + 5);
                cwd = g_cwd_buf; /* takes effect on the next turn */
            } else
                fprintf(stderr, "unknown command %s (/help)\n", line);
            continue;
        }
        char *goal = strdup(line);
        if (!goal) continue;
        lvc_agent_session_set_tools(sess, !casual_input(line));
        spin_start();
        if (lvc_agent_run_session(sess, host, model, goal, cwd,
                                  on_event, NULL) != 0)
            rc = 1;
        free(goal);
        if (line[0] != '/') hist_push(&hist, line);
        { /* omp-style persistence: autosave the transcript after each turn */
            const char *home = getenv("HOME");
            if (home && *home) {
                char ap[512], dp[512];
                snprintf(dp, sizeof dp, "%s/.predator", home);
                snprintf(ap, sizeof ap, "%s/.predator/last_session.jsonl", home);
                mkdir(dp, 0700);
                lvc_agent_session_save(sess, ap);
            }
        }
        free(line);
    }
    lvc_agent_session_free(sess);
    return rc;
}
