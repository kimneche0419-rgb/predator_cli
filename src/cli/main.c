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
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *PERSONA =
    "당신은 터미널 코딩 에이전트입니다. 작업 전 도구(list_dir/read_file)로 "
    "작업 공간을 먼저 확인하고, edit_file/run_shell로 실행합니다. "
    "설명은 간결한 한국어, 코드는 깔끔하게.\n"
    "규칙: 인사·일반 질문·상담에는 도구를 절대 쓰지 말고 바로 답변한다. "
    "파일 수정은 edit_file만 사용한다(run_shell 리다이렉션으로 파일을 "
    "통째로 쓰지 않는다). 사용자가 수정을 요청하지 않으면 read 이상 하지 않는다.";

static void on_sigint(int sig) { (void)sig; _exit(130); }

static char g_model_buf[256]; /* /model selection storage */
static char g_cwd_buf[1024];  /* /cwd selection storage */

/* ---- Claude Code-style UI ---- */

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
            "usage: %s [-m model] [-C dir] [-H host:port] [prompt]\n"
            "  prompt가 있으면 원샷 실행, 없으면 대화형 REPL\n"
            "  REPL 명령: /help /model [name] /cwd dir /reset /stat /quit\n",
            prog);
}

/* greetings and short chat don't need the tool loop */
static int casual_input(const char *s) {
    if (strlen(s) > 12) return 0;
    return strpbrk(s, "./\\`0123456789") == NULL;
}

/* goal = persona + task on a fresh session, raw input afterwards */
static char *build_goal(const char *input, int fresh, const char *cwd) {
    size_t n = strlen(input);
    if (!fresh) return strdup(input);
    char *g = malloc(strlen(PERSONA) + n + strlen(cwd) + 64);
    if (!g) return NULL;
    sprintf(g, "%s\n작업 공간: %s (도구 path는 이 디렉터리 기준 상대경로 권장)\n\n과제: %s",
            PERSONA, cwd, input);
    return g;
}

static void banner(const char *model, const char *host, const char *cwd) {
    if (!ui_color) {
        fprintf(stderr, "lvcli — model %s @ %s, cwd %s (/help)\n",
                model, host, cwd);
        return;
    }
    char l1[256], l2[512];
    snprintf(l1, sizeof l1, "✻ predator_cli");
    snprintf(l2, sizeof l2, "%s · %s", model, cwd);
    size_t w = strlen(l1) > strlen(l2) ? strlen(l1) : strlen(l2);
    if (w > 50) w = 50;
    if (strlen(l2) > w) l2[w] = 0;
    char edge[96], pad[96];
    snprintf(edge, sizeof edge, "╭%.*s╮", (int)(w + 2),
             "────────────────────────────────────────────────────────");
    memset(pad, ' ', sizeof pad - 1);
    fprintf(stderr,
            "%s%s%s\n"
            "%s│%s %s%s%s%.*s%s%s│%s\n"
            "%s│%s %s%s%s%.*s%s%s│%s\n"
            "%s%s%s\n"
            "  %s/help%s for commands · ctrl-c to exit\n",
            C_ACCENT, edge, C_RESET,
            C_ACCENT, C_RESET, C_BOLD, l1, C_RESET,
            (int)(w - strlen(l1) + 1), pad, C_ACCENT, C_RESET, C_RESET,
            C_ACCENT, C_RESET, C_DIM, l2, C_RESET,
            (int)(w - strlen(l2) + 1), pad, C_ACCENT, C_RESET, C_RESET,
            C_ACCENT, edge, C_RESET,
            C_DIM, C_RESET);
}

int main(int argc, char **argv) {
    const char *model = "qwen2.5-coder:3b";
    const char *cwd = ".";
    const char *host = "127.0.0.1:11434";
    const char *oneshot = NULL;
    ui_color = isatty(1) && isatty(2);
    {
        const char *ct = getenv("COLORTERM");
        ui_truecolor = ui_color && ct && strstr(ct, "truecolor") != NULL;
    }
    int opt;
    while ((opt = getopt(argc, argv, "m:C:H:h")) != -1) {
        switch (opt) {
        case 'm': model = optarg; break;
        case 'C': cwd = optarg; break;
        case 'H': host = optarg; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind < argc) oneshot = argv[optind];
    signal(SIGINT, on_sigint);

    lvc_agent_session *sess = lvc_agent_session_create();
    if (!sess) return 1;
    int rc = 0;

    if (oneshot) {
        char *goal = build_goal(oneshot, 1, cwd);
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
    char *line = NULL;
    size_t cap = 0;
    int first = 1; /* re-seed the persona after a fresh/cleared session */
    for (;;) {
        fprintf(stderr, "%s%s❯%s ", C_BOLD, C_ACCENT, C_RESET);
        fflush(stderr);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break; /* EOF: quit */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n) continue;
        if (line[0] == '/') {
            if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) {
                printf("%s✻ session ended%s\n", C_DIM, C_RESET);
                break;
            }
            else if (!strcmp(line, "/help"))
                fprintf(stderr, "/model [name] /cwd dir /reset /stat /quit\n");
            else if (!strcmp(line, "/reset")) {
                lvc_agent_session_clear(sess);
                first = 1;
                fprintf(stderr, "[session cleared]\n");
            } else if (!strcmp(line, "/stat")) {
                fprintf(stderr, "[messages: %zu]\n",
                        lvc_agent_session_len(sess));
            } else if (!strcmp(line, "/model") || !strncmp(line, "/model ", 7)) {
                const char *arg = line[7] ? line + 7 : NULL;
                if (!arg) { /* list installed models */
                    char **names = NULL;
                    size_t cnt = 0;
                    if (lvc_agent_list_models(host, &names, &cnt) == 0 && cnt) {
                        fprintf(stderr, "installed models:\n");
                        for (size_t i = 0; i < cnt; i++)
                            fprintf(stderr, "  %zu. %s%s\n", i + 1, names[i],
                                    strcmp(names[i], model)
                                        ? ""
                                        : "  ← current");
                        lvc_agent_free_models(names, cnt);
                        fprintf(stderr, "switch: /model <name> or /model <#>\n");
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
            } else if (!strncmp(line, "/cwd ", 5)) {
                snprintf(g_cwd_buf, sizeof g_cwd_buf, "%s", line + 5);
                cwd = g_cwd_buf; /* takes effect on the next turn */
            } else
                fprintf(stderr, "unknown command %s (/help)\n", line);
            continue;
        }
        char *goal = build_goal(line, first, cwd);
        if (!goal) continue;
        first = 0;
        lvc_agent_session_set_tools(sess, !casual_input(line));
        spin_start();
        if (lvc_agent_run_session(sess, host, model, goal, cwd,
                                  on_event, NULL) != 0)
            rc = 1;
        free(goal);
    }
    free(line);
    lvc_agent_session_free(sess);
    return rc;
}
