/* Ollama REST client (system libcurl). Streaming NDJSON /api/generate. */
#ifndef LVC_OLLAMA_H
#define LVC_OLLAMA_H

#include <stddef.h>
#include <stdbool.h>

#define LVC_OLLAMA_DEFAULT_URL "http://127.0.0.1:11434"

typedef struct {
    char *name;      /* model name */
    char *size_str;  /* human-readable size from API */
} lvc_model;

typedef struct {
    lvc_model *items;
    size_t count;
} lvc_model_list;

typedef void (*lvc_stream_cb)(const char *chunk, size_t len, void *user);

typedef struct {
    char base_url[256];
    int  last_http_status;
} lvc_ollama;

int  lvc_ollama_init(lvc_ollama *o, const char *base_url);
void lvc_ollama_free(lvc_ollama *o);

/* GET /api/tags. returns 0 ok, -1 transport, -2 HTTP error. list must be
 * freed with lvc_model_list_free. */
int  lvc_ollama_list(lvc_ollama *o, lvc_model_list *list);
void lvc_model_list_free(lvc_model_list *list);

/* POST /api/generate (stream). prompt is raw; cb receives each incremental
 * text chunk. returns 0 ok, -1 transport, -2 HTTP error. blocking. */
int  lvc_ollama_generate(lvc_ollama *o, const char *model, const char *prompt,
                        bool raw, lvc_stream_cb cb, void *user);

/* physical memory in bytes; used for model size recommendation */
bool lvc_sys_phys_mem(unsigned long long *out_bytes);

#endif
