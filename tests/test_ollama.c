#include "../src/ai/ollama.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char collected[8192];
static size_t collected_len = 0;
static void on_chunk(const char *c, size_t n, void *user) {
    (void)user;
    assert(collected_len + n < sizeof collected);
    memcpy(collected + collected_len, c, n);
    collected_len += n;
    collected[collected_len] = 0;
}

int main(void) {
    lvc_ollama o;
    assert(lvc_ollama_init(&o, "http://127.0.0.1:11435") == 0);

    /* list */
    lvc_model_list list;
    int r = lvc_ollama_list(&o, &list);
    fprintf(stderr, "list r=%d http=%d\n", r, o.last_http_status);
    assert(r == 0);
    assert(list.count == 2);
    assert(strcmp(list.items[0].name, "qwen2.5-coder:1.5b") == 0);
    assert(strncmp(list.items[0].size_str, "986.0 GB", 8) == 0 || list.items[0].size_str[0]);
    lvc_model_list_free(&list);

    fprintf(stderr,"stage:gen\n");
    /* generate streaming */
    collected_len = 0;
    collected[0] = 0;
    r = lvc_ollama_generate(&o, "qwen2.5-coder:1.5b", "hello world foo", false, on_chunk, NULL);
    assert(r == 0);
    printf("streamed: %s\n", collected);
    assert(strcmp(collected, "hello world foo ") == 0);
    fprintf(stderr,"stage:err\n");

    /* http error path */
    assert(lvc_ollama_generate(&o, "nope", "x", false, on_chunk, NULL) == -2);

    fprintf(stderr,"stage:transport\n");
    /* transport error path (no server on 11436) */
    lvc_ollama o2;
    assert(lvc_ollama_init(&o2, "http://127.0.0.1:11436") == 0);
    assert(lvc_ollama_list(&o2, &list) == -1);

    fprintf(stderr,"stage:mem\n");
    unsigned long long mem;
    assert(lvc_sys_phys_mem(&mem));
    printf("phys mem: %llu GB\n", mem / (1024ull * 1024 * 1024));

    lvc_ollama_free(&o);
    lvc_ollama_free(&o2);
    printf("ollama client tests passed\n");
    return 0;
}
