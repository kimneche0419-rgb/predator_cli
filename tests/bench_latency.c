/* micro-benchmark: keystroke latency through buffer + syntax respan.
 * Target: full edit->render-prep path < 16ms on Apple Silicon. */
#include "../src/core/buffer.h"
#include "../src/core/syntax.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void) {
    lvc_buf b;
    lvc_buf_init(&b, 1 << 16);
    /* ~100KB source file (worst realistic tab) */
    const char *line = "static int foo_bar(int argc, char **argv) { return argc + 1; }\n";
    while (lvc_buf_len(&b) < 100 * 1024)
        lvc_buf_insert(&b, lvc_buf_len(&b), (const uint8_t *)line, strlen(line));

    lvc_syntax s;
    lvc_syntax_init(&s);
    lvc_syntax_parse(&s, &b);

    /* single keystroke: insert + incremental respan */
    double worst = 0, sum = 0;
    enum { N = 200 };
    for (int i = 0; i < N; i++) {
        size_t pos = lvc_buf_len(&b) - 5;
        double t0 = ms();
        lvc_buf_insert(&b, pos, (const uint8_t *)"a", 1);
        lvc_syntax_edit(&s, &b, (uint32_t)pos, (uint32_t)pos, (uint32_t)(pos + 1));
        double dt = ms() - t0;
        sum += dt;
        if (dt > worst) worst = dt;
        lvc_buf_delete(&b, pos, 1);
        lvc_syntax_edit(&s, &b, (uint32_t)pos, (uint32_t)(pos + 1), (uint32_t)pos);
    }
    printf("keystroke path (100KB file): avg=%.3fms worst=%.3fms (n=%d, budget=16ms)\n",
           sum / N, worst, N);
    lvc_syntax_free(&s);
    lvc_buf_free(&b);
    return worst < 16 ? 0 : 1;
}
