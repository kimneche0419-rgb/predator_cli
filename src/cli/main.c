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
#include <signal.h>
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

static void print_trunc(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    fwrite(s, 1, n < max ? n : max, stdout);
    if (n > max) fputs("…", stdout);
}

static void on_event(void *user, lvc_agent_event ev, const char *text) {
    int color = *(int *)user;
    switch (ev) {
    case LVC_AGENT_TOKEN:
        fputs(text ? text : "", stdout);
        fflush(stdout);
        break;
    case LVC_AGENT_TOOL_CALL:
        printf("\n%s▶ ", color ? "\033[33m" : "");
        print_trunc(text, 200);
        printf("%s\n", color ? "\033[0m" : "");
        break;
    case LVC_AGENT_TOOL_RESULT: {
        const char *nl = text ? strchr(text, '\n') : NULL;
        size_t n = nl ? (size_t)(nl - text) : (text ? strlen(text) : 0);
        if (n > 200) n = 200;
        printf("%s◀ %.*s%s\n", color ? "\033[2m" : "", (int)n,
               text ? text : "", color ? "\033[0m" : "");
        break;
    }
    case LVC_AGENT_DONE:
        printf("\n");
        break;
    case LVC_AGENT_ERROR:
        printf("%s[error] %s%s\n", color ? "\033[31m" : "",
               text ? text : "", color ? "\033[0m" : "");
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

int main(int argc, char **argv) {
    const char *model = "qwen2.5-coder:3b";
    const char *cwd = ".";
    const char *host = "127.0.0.1:11434";
    const char *oneshot = NULL;
    int color = isatty(1);
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
        rc = lvc_agent_run_session(sess, host, model, goal, cwd,
                                   on_event, &color) == 0 ? 0 : 1;
        free(goal);
        lvc_agent_session_free(sess);
        return rc;
    }

    fprintf(stderr, "lvcli — model %s @ %s, cwd %s (/help)\n", model, host, cwd);
    char *line = NULL;
    size_t cap = 0;
    int first = 1; /* re-seed the persona after a fresh/cleared session */
    for (;;) {
        fprintf(stderr, "%s", color ? "\033[36m▸ \033[0m" : "▸ ");
        fflush(stderr);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break; /* EOF: quit */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n) continue;
        if (line[0] == '/') {
            if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
            else if (!strcmp(line, "/help"))
                fprintf(stderr, "/model [name] /cwd dir /reset /stat /quit\n");
            else if (!strcmp(line, "/reset")) {
                lvc_agent_session_clear(sess);
                first = 1;
                fprintf(stderr, "[session cleared]\n");
            } else if (!strcmp(line, "/stat"))
                fprintf(stderr, "[messages: %zu]\n",
                        lvc_agent_session_len(sess));
            else if (!strncmp(line, "/model ", 7)) {
                model = line + 7;
                fprintf(stderr, "[model: %s]\n", model);
            } else if (!strcmp(line, "/model"))
                fprintf(stderr, "[model: %s]\n", model);
            else if (!strncmp(line, "/cwd ", 5))
                cwd = line + 5; /* takes effect on the next turn */
            else
                fprintf(stderr, "unknown command %s (/help)\n", line);
            continue;
        }
        char *goal = build_goal(line, first, cwd);
        if (!goal) continue;
        first = 0;
        if (lvc_agent_run_session(sess, host, model, goal, cwd,
                                  on_event, &color) != 0)
            rc = 1;
        free(goal);
    }
    free(line);
    lvc_agent_session_free(sess);
    return rc;
}
