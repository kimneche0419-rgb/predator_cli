#include "io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static int buf_clear(lvc_buf *b) {
    size_t n = lvc_buf_len(b);
    return n == 0 ? 0 : (lvc_buf_delete(b, 0, n) >= 0 ? 0 : -1);
}

int lvc_io_load(lvc_buf *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (buf_clear(b) != 0) { fclose(f); return -1; }
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (lvc_buf_insert(b, lvc_buf_len(b), chunk, n) < 0) {
            fclose(f);
            return -1; /* invalid UTF-8 or OOM */
        }
    }
    int err = ferror(f);
    fclose(f);
    return err ? -1 : 0;
}

int lvc_io_save(lvc_buf *b, const char *path) {
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.lvc-tmp.%d", path, (int)getpid()) >= (int)sizeof tmp)
        return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    uint8_t chunk[65536];
    for (size_t pos = 0, n = lvc_buf_len(b); pos < n; ) {
        size_t take = n - pos < sizeof chunk ? n - pos : sizeof chunk;
        lvc_buf_read(b, pos, chunk, take);
        if (fwrite(chunk, 1, take, f) != take) { fclose(f); remove(tmp); return -1; }
        pos += take;
    }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}
