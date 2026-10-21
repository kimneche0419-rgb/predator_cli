/* Agent core: Ollama /api/chat tool-calling loop.
 *
 * Drives a model through up to LVC_AGENT_MAX_TOOL_ROUNDS tool rounds with
 * four workspace-sandboxed tools: list_dir, read_file, edit_file, run_shell.
 * Tool calls come from native message.tool_calls, or from fenced ```json
 * {"tool": ..., "args": {...}} blocks in assistant text (fallback parser for
 * models without native tool calling).
 */
#ifndef LVC_AGENT_H
#define LVC_AGENT_H

#include <stddef.h>

#define LVC_AGENT_MAX_TOOL_ROUNDS 10

typedef enum {
    LVC_AGENT_TOKEN,       /* incremental assistant text chunk */
    LVC_AGENT_TOOL_CALL,   /* text: "name: args" (args is raw JSON) */
    LVC_AGENT_TOOL_RESULT, /* text: tool output body */
    LVC_AGENT_DONE,        /* loop finished; text: "" */
    LVC_AGENT_ERROR        /* text: error message; lvc_agent_run returns -1 */
} lvc_agent_event;

typedef void (*lvc_agent_cb)(void *user, lvc_agent_event ev, const char *text);

/* Blocking tool loop against Ollama's streaming chat API (NDJSON).
 * host: "host:port" (e.g. "localhost:11434"); model: chat model id;
 * goal: user goal text sent as the first user message;
 * cwd: workspace root. Tool paths are normalized against cwd and rejected
 * when they resolve outside it (symlink aliases of cwd that really resolve
 * under it are accepted). run_shell executes with cwd as working directory.
 *
 * Every assistant text chunk is streamed via LVC_AGENT_TOKEN as it arrives;
 * each tool call emits LVC_AGENT_TOOL_CALL ("name: args") before execution
 * and LVC_AGENT_TOOL_RESULT (result body) after. When the model answers
 * without a tool call, the loop ends with LVC_AGENT_DONE.
 *
 * Returns 0 on a completed loop, -1 on failure (after LVC_AGENT_ERROR). */
int lvc_agent_run(const char *host, const char *model, const char *goal,
                  const char *cwd, lvc_agent_cb cb, void *user);

/* ---- multi-turn sessions (CLI) ---- */

typedef struct lvc_agent_session lvc_agent_session;

/* empty transcript */
lvc_agent_session *lvc_agent_session_create(void);
void lvc_agent_session_free(lvc_agent_session *s);

/* forget the whole transcript (manual /reset) */
void lvc_agent_session_clear(lvc_agent_session *s);

/* number of messages currently in the transcript (user+assistant+tool) */
size_t lvc_agent_session_len(const lvc_agent_session *s);

/* one user turn against the persisted transcript: like lvc_agent_run, but
 * the conversation (user/assistant/tool messages) survives across calls so
 * the model keeps context. After a DONE the transcript includes the full
 * exchange; after an error it may end mid-tool-round — call
 * lvc_agent_session_clear for a clean slate. Round limit per turn is
 * LVC_AGENT_MAX_TOOL_ROUNDS, as above. */
int lvc_agent_run_session(lvc_agent_session *s, const char *host,
                          const char *model, const char *goal,
                          const char *cwd, lvc_agent_cb cb, void *user);

/* enable/disable the tool loop for this session (default: enabled).
 * Disabled = pure chat: requests carry no tools and the model answers in
 * plain text. Handy for greetings/short questions. */
void lvc_agent_session_set_tools(lvc_agent_session *s, int enabled);

/* system prompt sent as the leading system message on every turn
 * (never stored in the transcript). NULL clears it. */
void lvc_agent_session_set_system(lvc_agent_session *s, const char *text);

/* persist the transcript as a JSON array file, and load one back
 * (replacing this session's transcript). returns 0/-1. */
int lvc_agent_session_save(const lvc_agent_session *s, const char *path);
int lvc_agent_session_load(lvc_agent_session *s, const char *path);

/* GET /api/tags: installed model names. out_names is a malloc'd array of
 * malloc'd strings — free with lvc_agent_free_models. returns 0/-1. */
int lvc_agent_list_models(const char *host, char ***out_names,
                          size_t *out_count);
void lvc_agent_free_models(char **names, size_t count);

/* register a directory the read_skill tool may load skills from
 * (up to 4 dirs; e.g. project .predator/skills and ~/.predator/skills) */
void lvc_agent_session_add_skills_dir(lvc_agent_session *s, const char *dir);

/* parse a markdown front-matter block ("---\nkey: value\n---") at the start
 * of text. fills name/description/globs ("" when absent), sets *always for
 * "alwaysApply: true". returns the byte offset of the body (after the
 * closing ---), or (long)-1 when no front-matter block is present. */
long lvc_parse_frontmatter(const char *text, size_t len, char *name,
                           size_t namesz, char *description, size_t descsz,
                           char *globs, size_t globsz, int *always);

#endif
