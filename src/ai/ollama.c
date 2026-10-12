#include "ollama.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <mach/mach.h>

typedef struct { char *data; size_t len, cap; } sbuf;

static void sbuf_init(sbuf *s) { s->data = NULL; s->len = s->cap = 0; }
static void sbuf_free(sbuf *s) { free(s->data); sbuf_init(s); }

static void sbuf_append(sbuf *s, const char *d, size_t n) {
    if (s->len + n + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4096;
        while (ncap < s->len + n + 1) ncap *= 2;
        char *p = realloc(s->data, ncap);
        if (!p) return;
        s->data = p; s->cap = ncap;
    }
    memcpy(s->data + s->len, d, n);
    s->len += n;
    s->data[s->len] = 0;
}

static size_t write_all(char *d, size_t sz, size_t nm, void *user) {
    sbuf_append((sbuf *)user, d, sz * nm);
    return sz * nm;
}

static int g_curl_refs = 0;

static int curl_ref(void) {
    if (g_curl_refs++ == 0 && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        g_curl_refs--;
        return -1;
    }
    return 0;
}

static void curl_unref(void) {
    /* intentionally no curl_global_cleanup: Apple's libcurl aborts at exit
     * when it runs; skipping it is per-spec (process exit cleans up). */
    g_curl_refs--;
}

int lvc_ollama_init(lvc_ollama *o, const char *base_url) {
    snprintf(o->base_url, sizeof o->base_url, "%s",
             base_url ? base_url : LVC_OLLAMA_DEFAULT_URL);
    o->last_http_status = 0;
    return curl_ref();
}

void lvc_ollama_free(lvc_ollama *o) { (void)o; curl_unref(); }

/* minimal JSON value extraction for flat objects: "key":"value" */
static bool json_str(const char *json, const char *key, char *out, size_t outlen) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i < outlen - 1) {
        out[i++] = *p == '\\' && p[1] ? *++p : *p;
        p++;
    }
    out[i] = 0;
    return true;
}

void lvc_model_list_free(lvc_model_list *l) {
    for (size_t i = 0; i < l->count; i++) { free(l->items[i].name); free(l->items[i].size_str); }
    free(l->items);
    l->items = NULL; l->count = 0;
}

int lvc_ollama_list(lvc_ollama *o, lvc_model_list *list) {
    list->items = NULL; list->count = 0;
    CURL *c = curl_easy_init();
    if (!c) return -1;
    sbuf body; sbuf_init(&body);
    char url[512];
    snprintf(url, sizeof url, "%s/api/tags", o->base_url);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_all);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode r = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    o->last_http_status = (int)http_code;
    curl_easy_cleanup(c);
    if (r != CURLE_OK) { sbuf_free(&body); return -1; }
    if (o->last_http_status != 200) { sbuf_free(&body); return -2; }
    if (!body.data) return 0;

    /* iterate "models":[{...},{...}] — split on "name":" occurrences */
    const char *p = body.data;
    while ((p = strstr(p, "\"name\":\"")) != NULL) {
        lvc_model m = {0};
        m.name = malloc(256);
        m.size_str = malloc(64);
        p += 8; /* skip `"name":"` */
        size_t i = 0;
        while (*p && *p != '"' && i < 255) m.name[i++] = *p++;
        m.name[i] = 0;
        /* size is a JSON number; find it and format */
        char sizepat[64];
        snprintf(sizepat, sizeof sizepat, "\"size\":");
        const char *sp = strstr(p, sizepat);
        if (sp) {
            unsigned long long szv = strtoull(sp + strlen(sizepat), NULL, 10);
            snprintf(m.size_str, 64, "%.1f GB", (double)szv / 1e9);
        } else m.size_str[0] = 0;
        lvc_model *ni = realloc(list->items, (list->count + 1) * sizeof(lvc_model));
        if (!ni) { free(m.name); free(m.size_str); break; }
        list->items = ni;
        list->items[list->count++] = m;
        p = strchr(p, '}') ? strchr(p, '}') + 1 : p + i;
    }
    sbuf_free(&body);
    return 0;
}

typedef struct { lvc_stream_cb cb; void *user; } stream_ctx;

static size_t stream_write(char *d, size_t sz, size_t nm, void *user) {
    stream_ctx *sc = user;
    size_t total = sz * nm;
    /* each NDJSON line: {"model":...,"response":"chunk","done":false} */
    static char linebuf[16384];
    static size_t fill = 0;
    const char *pp = d;
    size_t remaining = total;
    while (remaining--) {
        if (*pp == '\n') {
            linebuf[fill] = 0;
            char resp[8192];
            if (fill && json_str(linebuf, "response", resp, sizeof resp)) {
                size_t rl = strlen(resp);
                if (rl) sc->cb(resp, rl, sc->user);
            }
            fill = 0;
        } else if (fill < sizeof linebuf - 1) {
            linebuf[fill++] = *pp;
        }
        pp++;
    }
    return total;
}

int lvc_ollama_generate(lvc_ollama *o, const char *model, const char *prompt,
                        bool raw, lvc_stream_cb cb, void *user) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    char *escaped_model = curl_easy_escape(c, model, 0);
    char url[512];
    snprintf(url, sizeof url, "%s/api/generate", o->base_url);

    /* escape prompt into a JSON string */
    size_t plen = strlen(prompt);
    char *jp = malloc(plen * 6 + 16);
    size_t j = 0;
    jp[j++] = '"';
    for (size_t i = 0; i < plen; i++) {
        unsigned char ch = (unsigned char)prompt[i];
        if (ch == '"' || ch == '\\') { jp[j++] = '\\'; jp[j++] = ch; }
        else if (ch == '\n') { jp[j++] = '\\'; jp[j++] = 'n'; }
        else if (ch == '\t') { jp[j++] = '\\'; jp[j++] = 't'; }
        else if (ch == '\r') { jp[j++] = '\\'; jp[j++] = 'r'; }
        else jp[j++] = ch;
    }
    jp[j++] = '"';
    jp[j] = 0;

    char body[320 + j];
    if (raw)
        snprintf(body, sizeof body,
                 "{\"model\":\"%s\",\"prompt\":%s,\"raw\":true,\"stream\":true,"
                 "\"keep_alive\":\"30m\","
                 "\"options\":{\"num_predict\":64,\"temperature\":0.2,\"top_p\":0.9,"
                 "\"stop\":[\"<|endoftext|>\",\"<|fim_pad|>\"]}}",
                 model, jp);
    else
        snprintf(body, sizeof body,
                 "{\"model\":\"%s\",\"prompt\":%s,\"stream\":true,"
                 "\"keep_alive\":\"30m\","
                 "\"options\":{\"num_predict\":512,\"temperature\":0.2,"
                 "\"repeat_penalty\":1.15}}",
                 model, jp);

    stream_ctx sc = { cb, user };
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sc);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L); /* loopback: fail fast, never hang UI thread */
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 180L);
    CURLcode r = curl_easy_perform(c);
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    o->last_http_status = (int)http_code;
    curl_free(escaped_model);
    free(jp);
    curl_easy_cleanup(c);
    if (r != CURLE_OK) return -1;
    return o->last_http_status == 200 ? 0 : -2;
}

bool lvc_sys_phys_mem(unsigned long long *out) {
    size_t v = 0, len = sizeof v;
    if (sysctlbyname("hw.memsize", &v, &len, NULL, 0) != 0) return false;
    *out = v;
    return true;
}
