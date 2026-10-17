/* End-to-end test for the agent tool loop against tests/mock_ollama_chat.py
 * (scripted /api/chat on 127.0.0.1:11436 by default; no real Ollama needed).
 * Run from the repo root: /tmp/run_agent */
#include "../src/ai/agent.h"
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond) do {                                                  \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        exit(1);                                                          \
    }                                                                     \
} while (0)

#define MAX_EV 128
static struct { lvc_agent_event ev; char text[8192]; } g_ev[MAX_EV];
static size_t g_nev;

static void on_event(void *user, lvc_agent_event ev, const char *text) {
    (void)user;
    CHECK(g_nev < MAX_EV);
    g_ev[g_nev].ev = ev;
    snprintf(g_ev[g_nev].text, sizeof g_ev[g_nev].text, "%s", text ? text : "");
    g_nev++;
}

static pid_t start_mock(int port) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        char portstr[16];
        snprintf(portstr, sizeof portstr, "%d", port);
        execlp("python3", "python3", "tests/mock_ollama_chat.py", portstr, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(s >= 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int rc = connect(s, (struct sockaddr *)&a, sizeof a);
        close(s);
        if (rc == 0) return pid;
        usleep(100000);
    }
    (void)kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    fprintf(stderr, "mock server did not come up on port %d\n", port);
    exit(1);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strdup(buf);
}

int main(void) {
    int port = 11436;
    const char *pe = getenv("LVC_AGENT_TEST_PORT");
    if (pe && *pe) port = atoi(pe);

    CHECK(access("tests/mock_ollama_chat.py", R_OK) == 0); /* run from repo root */

    /* sandbox workspace */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/lvc_agent_testXXXXXX", tmp);
    CHECK(mkdtemp(dir) != NULL);

    pid_t mock = start_mock(port);

    char host[64];
    snprintf(host, sizeof host, "127.0.0.1:%d", port);
    int rc = lvc_agent_run(host, "qwen2.5-coder:3b-test",
                           "create created.txt, replace world with agent, read it back, "
                           "then try an out-of-workspace edit, then finish",
                           dir, on_event, NULL);
    CHECK(rc == 0);

    /* full event sequence: 3 scripted native rounds, fallback round (its
     * text is buffered, never streamed), escape round, then the final
     * answer emitted as one buffered TOKEN and DONE */
    static const lvc_agent_event exp[] = {
        LVC_AGENT_TOOL_CALL, LVC_AGENT_TOOL_RESULT,   /* run_shell */
        LVC_AGENT_TOOL_CALL, LVC_AGENT_TOOL_RESULT,   /* edit_file */
        LVC_AGENT_TOOL_CALL, LVC_AGENT_TOOL_RESULT,   /* read_file */
        LVC_AGENT_TOOL_CALL, LVC_AGENT_TOOL_RESULT,   /* fenced /etc/passwd */
        LVC_AGENT_TOOL_CALL, LVC_AGENT_TOOL_RESULT,   /* ../outside.txt */
        LVC_AGENT_TOKEN,                              /* final answer */
        LVC_AGENT_DONE,
    };
    CHECK(g_nev == sizeof exp / sizeof exp[0]);
    for (size_t i = 0; i < g_nev; i++) CHECK(g_ev[i].ev == exp[i]);

    CHECK(strncmp(g_ev[0].text, "run_shell: ", 11) == 0);
    CHECK(strstr(g_ev[0].text, "created.txt") != NULL);
    CHECK(strstr(g_ev[1].text, "created-by-agent") != NULL); /* shell output received */

    CHECK(strncmp(g_ev[2].text, "edit_file: ", 11) == 0);
    CHECK(strstr(g_ev[2].text, "created.txt") != NULL);
    CHECK(strcmp(g_ev[3].text, "ok") == 0); /* replacement applied */

    CHECK(strncmp(g_ev[4].text, "read_file: ", 11) == 0);
    CHECK(strcmp(g_ev[5].text, "hello agent") == 0);

    CHECK(strncmp(g_ev[6].text, "edit_file: ", 11) == 0);
    CHECK(strstr(g_ev[6].text, "/etc/passwd") != NULL);
    CHECK(strstr(g_ev[7].text, "escapes workspace") != NULL);

    CHECK(strncmp(g_ev[8].text, "edit_file: ", 11) == 0);
    CHECK(strstr(g_ev[8].text, "../outside.txt") != NULL);
    CHECK(strstr(g_ev[9].text, "escapes workspace") != NULL);

    /* the final answer arrives as one buffered TOKEN */
    char toks[16384];
    size_t tl = 0;
    for (size_t i = 0; i < g_nev; i++) {
        if (g_ev[i].ev != LVC_AGENT_TOKEN) continue;
        size_t n = strlen(g_ev[i].text);
        CHECK(tl + n < sizeof toks);
        memcpy(toks + tl, g_ev[i].text, n);
        tl += n;
    }
    toks[tl] = 0;
    char want[16384];
    CHECK(snprintf(want, sizeof want, "Done: created.txt now says hello agent.") >= 0);
    CHECK(strcmp(toks, want) == 0);
    CHECK(g_ev[g_nev - 1].ev == LVC_AGENT_DONE);
    CHECK(strcmp(g_ev[g_nev - 1].text, "") == 0);

    /* the edit_file round really rewrote the file in the sandbox */
    char fpath[1200];
    snprintf(fpath, sizeof fpath, "%s/created.txt", dir);
    char *body = slurp(fpath);
    CHECK(strcmp(body, "hello agent") == 0);
    free(body);

    /* no server: transport error surfaces as ERROR event + -1 */
    g_nev = 0;
    rc = lvc_agent_run("127.0.0.1:9", "m", "goal", dir, on_event, NULL);
    CHECK(rc == -1);
    CHECK(g_nev == 1 && g_ev[0].ev == LVC_AGENT_ERROR);

    (void)kill(mock, SIGTERM);
    waitpid(mock, NULL, 0);
    unlink(fpath);
    rmdir(dir);
    printf("agent tests passed\n");
    return 0;
}
