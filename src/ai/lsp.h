/* LSP client: JSON-RPC over stdio with Content-Length framing.
 * Spawns the server (e.g. clangd), full-sync document lifecycle. */
#ifndef LVC_LSP_H
#define LVC_LSP_H

#include <stddef.h>
#include "../core/buffer.h"

typedef struct {
    int   pid;          /* server pid, -1 if not running */
    int   fd_in;        /* write to server stdin */
    int   fd_out;       /* read from server stdout */
    int   fd_err;
    int   next_id;
    char *last_diagnostics; /* heap copy of latest publishDiagnostics JSON, or NULL */
    size_t diag_len;
} lvc_lsp;

/* spawn server with argv (NULL-terminated). root_uri e.g. "file:///path". */
int  lvc_lsp_start(lvc_lsp *s, char *const argv[], const char *root_uri,
                   const char *path, const lvc_buf *b);
/* full-sync didChange; call after edits. */
int  lvc_lsp_notify_change(lvc_lsp *s, const char *path, lvc_buf *b);
/* send request, block until response with matching id arrives.
 * notifications (no id) are consumed; publishDiagnostics captured.
 * returns malloc'd result JSON (caller frees) or NULL. */
char *lvc_lsp_request(lvc_lsp *s, const char *method, const char *params_json);
/* non-blocking poll: capture publishDiagnostics if pending. returns 1 if captured. */
int  lvc_lsp_poll(lvc_lsp *s);
int  lvc_lsp_stop(lvc_lsp *s);

/* framing helpers (also unit-testable without a process): write framed
 * message to fd; read one framed message from fd into *out (malloc). */
int  lvc_lsp_write_frame(int fd, const char *body);
int  lvc_lsp_read_frame(int fd, char **out);

#endif
