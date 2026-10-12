/* LVC editor shell: AppKit window + custom CoreText-backed editor view
 * bound directly to the C gap-buffer core. */
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <string.h>
#include <pthread.h>
#include "../core/buffer.h"
#include "../core/io.h"
#include "../core/undo.h"
#include "../core/syntax.h"
#include "../core/diff.h"
#include "../ai/ollama.h"
#include "../ai/lsp.h"
#include "../ai/fim.h"
#include "../ai/agent.h"
#import "metal_view.h"
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>

static const char *k_scratch_path = "/tmp/lvc_scratch.c";
const char *lvc_scratch_path(void) { return k_scratch_path; }
static char g_open_path[4096] = "/tmp/lvc_scratch.c"; /* current file */
static char g_fim_log_path[4096] = "/tmp/lvc_fim_suggestions.jsonl"; /* launch: $HOME/.lvc/suggestions.jsonl */
static lvc_ollama *g_ollama = NULL;   /* set by AppDelegate at launch */
static NSString *g_model = nil;       /* shared chat/refactor/FIM model */
static NSString *g_query = nil;       /* last chat input / refactor instruction */
static NSString *g_skill = nil;       /* active skill header shown in sidebar */
static NSString *g_pending_refactor = nil; /* rewritten full text awaiting apply */
static lvc_lsp *g_lsp = NULL;         /* optional clangd connection */

@class AppDelegate;

@interface EditorView : NSView
@property (assign) lvc_buf *buf;
@property (assign) lvc_undo *undo;
@property (assign) lvc_syntax *syntax;
@property (assign) size_t cursor;
@property (assign) CGFloat line_height;
@property (assign) CTFontRef font;
@property (retain) NSString *ghost;              /* pending suggestion (may be multi-line) */
@property (retain) NSString *ghost_full;         /* full original suggestion for telemetry */
@property (retain) NSString *ghost_ctx;          /* text before cursor at request time */
@property (assign) BOOL ghost_partial;           /* some lines already accepted */
@property (retain) NSString *last_prompt;        /* dedup: prompt of the shown ghost */
@property (assign) NSUInteger gen;               /* bumps on dismiss/schedule; stale drops */
@property (retain) NSTimer *fim_timer;           /* debounce timer */
- (void)scheduleCompletion;
- (void)fireCompletion:(NSTimer *)t;
- (void)acceptGhost;
- (void)dismissGhost;
@end

static NSColor *color_for_kind(lvc_token_kind k) {
    switch (k) {
    case LVC_TK_KEYWORD:  return [NSColor colorWithSRGBRed:0.85 green:0.50 blue:0.90 alpha:1];
    case LVC_TK_STRING:   return [NSColor colorWithSRGBRed:0.75 green:0.90 blue:0.50 alpha:1];
    case LVC_TK_COMMENT:  return [NSColor colorWithSRGBRed:0.42 green:0.48 blue:0.42 alpha:1];
    case LVC_TK_NUMBER:   return [NSColor colorWithSRGBRed:0.90 green:0.70 blue:0.45 alpha:1];
    case LVC_TK_TYPE:     return [NSColor colorWithSRGBRed:0.50 green:0.80 blue:0.90 alpha:1];
    default:              return [NSColor colorWithSRGBRed:0.88 green:0.88 blue:0.88 alpha:1];
    }
}

@implementation EditorView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _font = CTFontCreateWithName(CFSTR("Menlo"), 13.0, NULL);
        _line_height = ceil(CTFontGetAscent(_font) + CTFontGetDescent(_font) + 2);
        _cursor = 0;
    }
    return self;
}

- (void)dealloc {
    if (_font) CFRelease(_font);
    [super dealloc];
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }

static BOOL g_lsp_dirty = NO;        /* unsent buffer change for clangd */
static lvc_buf *g_edit_buf = NULL;   /* buffer for the deferred change */

- (void)applyEdit:(uint32_t)start oldEnd:(uint32_t)oldEnd newEnd:(uint32_t)newEnd {
    lvc_syntax_edit(self.syntax, self.buf, start, oldEnd, newEnd);
    if (g_lsp) { g_lsp_dirty = YES; g_edit_buf = self.buf; }
    [self setNeedsDisplay:YES];
}

- (void)insertBytes:(const uint8_t *)bytes len:(size_t)len {
    uint32_t start = (uint32_t)self.cursor;
    ptrdiff_t r = lvc_buf_insert(self.buf, self.cursor, bytes, len);
    if (r > 0) {
        lvc_undo_push(self.undo, LVC_OP_INSERT, self.cursor, bytes, len,
                      self.undo->next_group);
        self.cursor += (size_t)r;
        [self applyEdit:start oldEnd:start newEnd:(uint32_t)self.cursor];
    }
}

- (void)deleteRange:(size_t)pos len:(size_t)n {
    uint32_t start = (uint32_t)pos;
    ptrdiff_t r = lvc_undo_delete(self.undo, self.buf, pos, n, self.undo->next_group);
    if (r > 0) [self applyEdit:start oldEnd:(uint32_t)(pos + n) newEnd:start];
}

#pragma mark FIM completion

typedef struct {
    EditorView *v;
    char *ctx_tail;              /* last lines before cursor at request time */
    NSMutableData *acc;          /* raw streamed suggestion bytes */
    NSUInteger gen;
} fim_ctx;

static void on_fim_stream(const char *chunk, size_t len, void *user) {
    fim_ctx *fc = user;
    [fc->acc appendBytes:chunk length:len];
}

/* language tag for telemetry from the open file extension */
static const char *lang_from_path(void) {
    const char *dot = strrchr(g_open_path, '.');
    if (!dot) return "text";
    if (!strcmp(dot, ".c") || !strcmp(dot, ".h")) return "c";
    if (!strcmp(dot, ".py")) return "python";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "html";
    if (!strcmp(dot, ".js")) return "js";
    if (!strcmp(dot, ".ts")) return "ts";
    return "text";
}

/* extract declarations from the local headers the open file includes, so the
 * FIM prefix carries cross-file symbol context. malloc'd or NULL. */
static char *collect_header_prototypes(lvc_buf *buf) {
    size_t n = lvc_buf_len(buf);
    if (n > 65536) n = 65536;
    char *text = malloc(n + 1);
    if (!text) return NULL;
    lvc_buf_read(buf, 0, (uint8_t *)text, n);
    text[n] = 0;

    char dir[4096];
    snprintf(dir, sizeof dir, "%s", g_open_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");

    char *out = malloc(1400);
    if (!out) { free(text); return NULL; }
    size_t w = 0;
    const char *p = text;
    while (w < 1200 && (p = strstr(p, "#include \"")) != NULL) {
        p += 10;
        const char *e = strchr(p, '"');
        if (!e || e == p || e - p > 200) { continue; }
        char name[208];
        memcpy(name, p, (size_t)(e - p)); name[e - p] = 0;
        p = e + 1;
        char path[4300];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        while (w < 1200 && fgets(line, sizeof line, f)) {
            size_t ll = strlen(line);
            while (ll && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = 0;
            if (!ll) continue;
            int is_proto = line[0] != '#' && strchr(line, '(') != NULL &&
                           line[ll - 1] == ';' && !strstr(line, "typedef") &&
                           !strchr(line, '{');
            int is_def = strncmp(line, "#define ", 8) == 0 && ll < 120;
            if (!is_proto && !is_def) continue;
            if (w + ll + 2 >= 1390) break;
            memcpy(out + w, line, ll); w += ll;
            out[w++] = '\n';
        }
        fclose(f);
    }
    free(text);
    if (w == 0) { free(out); return NULL; }
    out[w] = 0;
    return out;
}

static void *fim_thread(void *arg) {
    EditorView *v = (__bridge void *)arg; /* retained via CFBridgingRetain */
    size_t plen = 0;
    char *ctx = collect_header_prototypes(v->_buf);
    size_t sfx = lvc_fim_suffix_bound(v->_buf, v->_cursor, 1024);
    if (sfx < 64) sfx = 64;
    char *prompt = lvc_fim_build_prompt_ex(v->_buf, v->_cursor, 4096, sfx,
                                           ctx, &plen);
    free(ctx);
    if (prompt) {
        NSString *ps = [[NSString alloc] initWithBytes:prompt length:plen
                                              encoding:NSUTF8StringEncoding];
        BOOL dup = v.ghost_full != nil && [ps isEqualToString:v.last_prompt];
        if (!dup) {
            v.last_prompt = ps;
            fim_ctx fc;
            fc.v = v;
            fc.acc = [NSMutableData data];
            fc.gen = v.gen;
            size_t start = v->_cursor > 512 ? v->_cursor - 512 : 0;
            size_t len = v->_cursor - start;
            fc.ctx_tail = malloc(len + 1);
            lvc_buf_read(v->_buf, start, (uint8_t *)fc.ctx_tail, len);
            fc.ctx_tail[len] = 0;

            const char *model = g_model ? g_model.UTF8String
                                        : "lvc-coder:latest";
            lvc_ollama_generate(g_ollama, model, prompt, true, on_fim_stream, &fc);
            free(fc.ctx_tail);

            /* publish only if no newer keystroke dismissed the request */
            if (fc.gen == v.gen && !v.ghost_full) {
                NSString *sug = [[NSString alloc] initWithData:fc.acc
                                                      encoding:NSUTF8StringEncoding];
                while ([sug hasSuffix:@"\n"] || [sug hasSuffix:@" "] ||
                       [sug hasSuffix:@"\t"] || [sug hasSuffix:@"\r"])
                    sug = [sug substringToIndex:sug.length - 1];
                if (sug.length && lvc_fim_sane(sug.UTF8String, fc.ctx_tail)) {
                    NSUInteger g = fc.gen;
                    NSString *ctxs = [NSString stringWithUTF8String:
                        fc.ctx_tail ? fc.ctx_tail : ""];
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (v.gen != g || v.ghost_full) return;
                        v.ghost_full = sug;
                        v.ghost = sug;
                        v.ghost_ctx = ctxs;
                        v.ghost_partial = NO;
                        [v setNeedsDisplay:YES];
                    });
                }
            }
        }
        free(prompt);
    }
    CFBridgingRelease(v);
    return NULL;
}

- (void)scheduleCompletion {
    [self dismissGhost];
    self.gen++;
    [self.fim_timer invalidate];
    self.fim_timer = [NSTimer timerWithTimeInterval:0.3 target:self
        selector:@selector(fireCompletion:) userInfo:nil repeats:NO];
    [[NSRunLoop mainRunLoop] addTimer:self.fim_timer forMode:NSDefaultRunLoopMode];
}

- (void)fireCompletion:(NSTimer *)t {
    (void)t;
    if (!g_ollama) return;
    /* cheap trigger gate: no suggestions on blank-line runs */
    if (self.cursor >= 2 && lvc_buf_get(self.buf, self.cursor - 1) == '\n' &&
        lvc_buf_get(self.buf, self.cursor - 2) == '\n') return;
    pthread_t th;
    pthread_create(&th, NULL, fim_thread, (void *)CFBridgingRetain(self));
    pthread_detach(th);
}

- (void)acceptGhost {
    if (!self.ghost.length) return;
    NSRange nl = [self.ghost rangeOfString:@"\n"];
    NSString *piece = nl.location == NSNotFound
        ? self.ghost : [self.ghost substringToIndex:nl.location + 1];
    const char *utf8 = piece.UTF8String;
    [self insertBytes:(const uint8_t *)utf8 len:strlen(utf8)];
    self.ghost = nl.location == NSNotFound
        ? nil : [self.ghost substringFromIndex:nl.location + 1];
    if (self.ghost.length) {
        self.ghost_partial = YES;
    } else {
        /* final piece: count the whole suggestion as accepted */
        lvc_fim_log(g_fim_log_path, self.ghost_full.UTF8String, 1,
                    lang_from_path(), self.ghost_ctx.UTF8String);
        self.ghost_full = nil;
        [self scheduleCompletion]; /* keep suggesting at the new cursor */
    }
}

- (void)dismissGhost {
    if (self.ghost_full) {
        /* accepted flag covers partial accepts; untouched ghost = rejected */
        lvc_fim_log(g_fim_log_path, self.ghost_full.UTF8String,
                    self.ghost_partial ? 1 : 0, lang_from_path(),
                    self.ghost_ctx.UTF8String);
        self.ghost_full = nil;
    }
    self.ghost = nil;
    self.gen++;
    [self setNeedsDisplay:YES];
}

#pragma mark inline refactor (LLM rewrite -> diff -> apply)

static char *g_old_snapshot = NULL; /* buffer text before refactor */

static void on_refactor_stream(const char *chunk, size_t len, void *user) {
    (void)user;
    /* accumulate into g_pending_refactor */
    NSString *s = [[NSString alloc] initWithBytes:chunk length:len
                                         encoding:NSUTF8StringEncoding];
    dispatch_async(dispatch_get_main_queue(), ^{
        g_pending_refactor = g_pending_refactor ? [g_pending_refactor stringByAppendingString:s] : s;
    });
}

static void *refactor_thread(void *arg) {
    EditorView *v = (__bridge void *)arg;
    /* snapshot old text */
    size_t n = lvc_buf_len(v->_buf);
    free(g_old_snapshot);
    g_old_snapshot = malloc(n + 1);
    lvc_buf_read(v->_buf, 0, (uint8_t *)g_old_snapshot, n);
    g_old_snapshot[n] = 0;

    NSString *prompt = [NSString stringWithFormat:
        @"Rewrite the following C file according to the instruction. "
         "Output ONLY the full rewritten file, no explanations.\n"
         "Instruction: %@\n```\n%s\n```", g_query ?: @"", g_old_snapshot];
    g_pending_refactor = nil;
    lvc_ollama_generate(g_ollama, g_model.UTF8String, prompt.UTF8String,
                        false, on_refactor_stream, v);
    dispatch_async(dispatch_get_main_queue(), ^{
        AppDelegate *del = (AppDelegate *)[[NSApplication sharedApplication] delegate];
        /* strip markdown fences if present */
        NSString *t = g_pending_refactor ?: @"";
        NSRange f = [t rangeOfString:@"```"];
        if (f.location != NSNotFound) {
            NSRange e = [t rangeOfString:@"```" options:0 range:NSMakeRange(f.location + 3, t.length - f.location - 3)];
            t = e.location != NSNotFound
                ? [t substringWithRange:NSMakeRange(f.location + 3, e.location - f.location - 3)]
                : [t substringFromIndex:f.location + 3];
            t = [t stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        }
        /* strip leading language tag line */
        NSRange nl = [t rangeOfString:@"\n"];
        if (nl.location != NSNotFound) {
            NSString *first = [t substringToIndex:nl.location];
            if ([first isEqualToString:@"c"] || [first isEqualToString:@"cpp"] || [first hasPrefix:@"c++"])
                t = [t substringFromIndex:nl.location + 1];
        }
        lvc_diff d;
        if (lvc_diff_lines(g_old_snapshot, t.UTF8String, &d) == 0) {
            [del appendChat:[NSString stringWithFormat:
                @"\n[refactor: %zu hunk(s)]\n", d.count]];
            for (size_t i = 0; i < d.count; i++) {
                [del appendChat:[NSString stringWithFormat:@"@@ -%zu +%zu @@\n",
                    d.hunks[i].old_start, d.hunks[i].new_start]];
                for (size_t k = 0; k < d.hunks[i].del_count; k++)
                    [del appendChat:[NSString stringWithFormat:@"-%s", d.hunks[i].del_lines[k]]];
                for (size_t k = 0; k < d.hunks[i].ins_count; k++)
                    [del appendChat:[NSString stringWithFormat:@"+%s", d.hunks[i].ins_lines[k]]];
            }
            lvc_diff_free(&d);
            [del appendChat:@"[Cmd+Shift+A apply / Cmd+Shift+D discard]\n"];
        }
    });
    return NULL;
}

- (void)startRefactor {
    if (!g_ollama || !g_model) return;
    pthread_t th;
    pthread_create(&th, NULL, refactor_thread, self);
    pthread_detach(th);
}

- (void)applyRefactor {
    AppDelegate *del = (AppDelegate *)[[NSApplication sharedApplication] delegate];
    if (!g_pending_refactor || !g_old_snapshot) return;
    lvc_diff d;
    if (lvc_diff_lines(g_old_snapshot, g_pending_refactor.UTF8String, &d) != 0) return;

    /* byte offset of each old line start (snapshot == current buffer) */
    size_t n = lvc_buf_len(self.buf);
    size_t line_cap = 64, line_count = 1;
    size_t *line_off = malloc(line_cap * sizeof(size_t));
    line_off[0] = 0;
    for (size_t i = 0; i < n; i++) {
        if (lvc_buf_get(self.buf, i) == '\n') {
            if (++line_count > line_cap) {
                line_cap *= 2;
                line_off = realloc(line_off, line_cap * sizeof(size_t));
            }
            line_off[line_count - 1] = i + 1;
        }
    }

    /* apply hunks in order, tracking byte delta */
    ptrdiff_t delta = 0;
    for (size_t i = 0; i < d.count; i++) {
        lvc_hunk *h = &d.hunks[i];
        size_t pos = (size_t)((ptrdiff_t)line_off[h->old_start] + delta);
        size_t del_bytes = 0;
        for (size_t k = 0; k < h->del_count; k++) del_bytes += strlen(h->del_lines[k]);
        if (h->del_count) {
            ptrdiff_t r = lvc_undo_delete(self.undo, self.buf, pos, del_bytes,
                                          self.undo->next_group);
            if (r < 0) { free(line_off); lvc_diff_free(&d); return; }
        }
        size_t ins_bytes = 0;
        for (size_t k = 0; k < h->ins_count; k++) ins_bytes += strlen(h->ins_lines[k]);
        if (h->ins_count) {
            size_t total = ins_bytes;
            char *joined = malloc(total + 1);
            joined[0] = 0;
            for (size_t k = 0; k < h->ins_count; k++) strcat(joined, h->ins_lines[k]);
            ptrdiff_t r = lvc_buf_insert(self.buf, pos, (const uint8_t *)joined, total);
            if (r > 0)
                lvc_undo_push(self.undo, LVC_OP_INSERT, pos, (const uint8_t *)joined,
                              (size_t)r, self.undo->next_group);
            free(joined);
            if (r < 0) { free(line_off); lvc_diff_free(&d); return; }
        }
        delta += (ptrdiff_t)ins_bytes - (ptrdiff_t)del_bytes;
    }
    self.cursor = lvc_buf_len(self.buf);
    lvc_syntax_parse(self.syntax, self.buf);
    [self setNeedsDisplay:YES];
    [del appendChat:@"[refactor applied — undo available]\n"];
    lvc_diff_free(&d);
    free(line_off);
    g_pending_refactor = nil;
}

- (void)discardRefactor {
    AppDelegate *del = (AppDelegate *)[[NSApplication sharedApplication] delegate];
    g_pending_refactor = nil;
    [del appendChat:@"[refactor discarded]\n"];
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = event.characters;
    if (!chars.length) return;
    unichar c = [chars characterAtIndex:0];
    NSUInteger flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;

    if (flags & NSEventModifierFlagCommand) {
        if (flags & NSEventModifierFlagShift) {
            if (c == 'R') { [self startRefactor]; return; }
            if (c == 'A') { [self applyRefactor]; return; }
            if (c == 'D') { [self discardRefactor]; return; }
        }
        if (c == 'z') {
            if (flags & NSEventModifierFlagShift) lvc_undo_redo(self.undo, self.buf);
            else lvc_undo_undo(self.undo, self.buf);
            [self setNeedsDisplay:YES];
        } else if (c == 's') {
            lvc_io_save(self.buf, k_scratch_path);
        }
        return;
    }
    switch (c) {
    case NSDeleteCharacter: {
        if (self.cursor > 0) {
            [self dismissGhost];
            size_t prev = self.cursor - 1;
            while (prev > 0 && (lvc_buf_get(self.buf, prev) & 0xC0) == 0x80) prev--;
            [self deleteRange:prev len:self.cursor - prev];
        }
        return;
    }
    case 27: /* escape */
        [self dismissGhost];
        return;
    case 9:  /* tab */
        if (self.ghost.length) { [self acceptGhost]; return; }
        [self insertBytes:(const uint8_t *)"    " len:4];
        return;
    case '\r': [self dismissGhost]; [self insertBytes:(const uint8_t *)"\n" len:1]; return;
    default: break;
    }
    if (c >= 0xF700) { /* function/arrow keys */
        size_t n = lvc_buf_len(self.buf), cur = self.cursor;
        if (c == NSLeftArrowFunctionKey || c == NSRightArrowFunctionKey) {
            if (c == NSLeftArrowFunctionKey) {
                if (cur == 0) return;
                size_t p = cur - 1;
                while (p > 0 && (lvc_buf_get(self.buf, p) & 0xC0) == 0x80) p--;
                self.cursor = p;
            } else {
                if (cur >= n) return;
                size_t p = cur + 1;
                while (p < n && (lvc_buf_get(self.buf, p) & 0xC0) == 0x80) p++;
                self.cursor = p;
            }
        } else if (c == NSUpArrowFunctionKey || c == NSDownArrowFunctionKey) {
            /* column is a byte offset within the line */
            size_t ls = cur;
            while (ls > 0 && lvc_buf_get(self.buf, ls - 1) != '\n') ls--;
            size_t col = cur - ls, target;
            if (c == NSUpArrowFunctionKey) {
                if (ls == 0) return;
                size_t ps = ls - 1;
                while (ps > 0 && lvc_buf_get(self.buf, ps - 1) != '\n') ps--;
                size_t plen = (ls - 1) - ps;
                target = ps + (col < plen ? col : plen);
            } else {
                size_t le = cur;
                while (le < n && lvc_buf_get(self.buf, le) != '\n') le++;
                if (le >= n) return; /* already on the last line */
                size_t ns = le + 1, ne = ns;
                while (ne < n && lvc_buf_get(self.buf, ne) != '\n') ne++;
                size_t nlen = ne - ns;
                target = ns + (col < nlen ? col : nlen);
            }
            self.cursor = target;
        } else {
            return; /* other function keys */
        }
        [self dismissGhost];
        [self setNeedsDisplay:YES];
        return;
    }
    const char *utf8 = chars.UTF8String;
    [self insertBytes:(const uint8_t *)utf8 len:strlen(utf8)];
    [self scheduleCompletion];
}

- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithCalibratedWhite:0.035 alpha:1] set]; /* near-black editor bg */
    NSRectFill(self.bounds);

    CGFloat line_w = self.bounds.size.width - 20;

    /* gather highlight spans once per frame */
    lvc_span spans[512];
    size_t nspans = lvc_syntax_spans(self.syntax, spans, 512);
    size_t span_i = 0;

    /* render line by line from the gap buffer */
    size_t n = lvc_buf_len(self.buf);
    size_t line_start = 0;
    CGFloat y = 8;
    NSMutableAttributedString *attr = nil;
    while (line_start <= n) {
        size_t line_end = line_start;
        char chunk[4096];
        size_t take = 0;
        while (line_end < n && take < sizeof chunk - 1) {
            uint8_t ch = lvc_buf_get(self.buf, line_end);
            chunk[take++] = (char)ch;
            line_end++;
            if (ch == '\n') break;
        }
        chunk[take] = 0;
        attr = [[NSMutableAttributedString alloc]
                 initWithString:[NSString stringWithUTF8String:chunk]
                     attributes:@{
            NSForegroundColorAttributeName:
                [NSColor colorWithSRGBRed:0.88 green:0.88 blue:0.88 alpha:1],
        }];
        /* byte -> UTF-16 offset map (NSAttributedString ranges are UTF-16) */
        NSString *line = attr.string;
        NSUInteger b2u[4097];
        {
            NSUInteger u = 0, b = 0;
            while (u < line.length && b < take) {
                b2u[b] = u;
                NSRange r = [line rangeOfComposedCharacterSequenceAtIndex:u];
                NSUInteger clen = [[line substringWithRange:r]
                    lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
                if (clen == 0) clen = 1;
                b += clen;
                u = r.location + r.length;
            }
            b2u[b > take ? take : b] = line.length;
        }
        /* color runs intersecting spans */
        for (size_t i = 0; i < take; ) {
            size_t byte_abs = line_start + i;
            lvc_token_kind kind = LVC_TK_PLAIN;
            while (span_i < nspans && spans[span_i].end <= byte_abs) span_i++;
            if (span_i < nspans && spans[span_i].start <= byte_abs &&
                byte_abs < spans[span_i].end) kind = spans[span_i].kind;
            size_t run_end = i + 1;
            if (kind != LVC_TK_PLAIN) {
                while (run_end < take && line_start + run_end < spans[span_i].end) run_end++;
            }
            if (kind != LVC_TK_PLAIN) {
                NSUInteger u0 = b2u[i], u1 = b2u[run_end > take ? take : run_end];
                if (u1 > line.length) u1 = line.length;
                if (u1 > u0)
                    [attr addAttribute:NSForegroundColorAttributeName
                                 value:color_for_kind(kind)
                                 range:NSMakeRange(u0, u1 - u0)];
            }
            i = run_end;
        }
        /* draw with AppKit (CTLineDraw garbles in window contexts on this system) */
        [attr drawAtPoint:NSMakePoint(10, y)];
        [attr release];
        y += self.line_height;
        if (y > self.bounds.size.height) break;
        if (line_end == line_start) break;
        line_start = line_end;
    }

    /* caret: precise x via CTLine measurement of the cursor-line prefix */
    size_t cur_line_start = 0, cursor_line_idx = 0;
    for (size_t i = 0; i < self.cursor; i++) {
        if (lvc_buf_get(self.buf, i) == '\n') { cur_line_start = i + 1; cursor_line_idx++; }
    }
    size_t take = self.cursor - cur_line_start;
    uint8_t *pre = malloc(take ? take : 1);
    if (take) lvc_buf_read(self.buf, cur_line_start, pre, take);
    CFStringRef cfpre = CFStringCreateWithBytes(
        kCFAllocatorDefault, pre, (CFIndex)take, kCFStringEncodingUTF8, false);
    CTLineRef pre_line = CTLineCreateWithAttributedString(
        CFAttributedStringCreate(kCFAllocatorDefault, cfpre,
            (__bridge CFDictionaryRef)@{ (id)kCTFontAttributeName:
                (__bridge id)self.font }));
    CGFloat xoff = pre_line ? CTLineGetTypographicBounds(pre_line, NULL, NULL, NULL) : 0;
    if (pre_line) CFRelease(pre_line);
    CFRelease(cfpre); free(pre);

    CGFloat caret_y = 8 + cursor_line_idx * self.line_height;
    [[NSColor colorWithCalibratedWhite:0.92 alpha:0.95] set];
    NSRectFill(NSMakeRect(10 + xoff, caret_y + 2, 2, self.line_height - 4));

    /* ghost text: gray continuation lines after the caret */
    if (self.ghost.length) {
        NSDictionary *ga = @{ NSFontAttributeName: [NSFont fontWithName:@"Menlo" size:13],
                              NSForegroundColorAttributeName:
                                [NSColor colorWithWhite:0.55 alpha:0.8] };
        NSArray *ghost_lines = [self.ghost componentsSeparatedByString:@"\n"];
        for (NSUInteger gi = 0; gi < ghost_lines.count; gi++) {
            NSString *gline = ghost_lines[gi] ?: @"";
            [gline drawAtPoint:NSMakePoint(gi == 0 ? 10 + xoff : 10,
                     caret_y + 4 + gi * self.line_height)
                withAttributes:ga];
        }
    }

    (void)line_w;
}
@end

@interface FileNode : NSObject
@property (copy) NSString *name, *path;
@property BOOL isDir, loaded;
@property (retain) NSMutableArray<FileNode *> *children;
+ (FileNode *)nodeWithPath:(NSString *)p name:(NSString *)n isDir:(BOOL)d;
@end
@implementation FileNode
+ (FileNode *)nodeWithPath:(NSString *)p name:(NSString *)n isDir:(BOOL)d {
    FileNode *f = [FileNode new];
    f.path = p; f.name = n; f.isDir = d; f.children = [NSMutableArray array];
    return f;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (retain) NSWindow *window;
@property (retain) EditorView *view;
@property (assign) lvc_buf buf;
@property (assign) lvc_undo undo;
@property (assign) lvc_syntax syntax;
@property (assign) lvc_ollama ollama;
@property (retain) NSTextView *sidebar;
@property (retain) NSPopUpButton *model_popup;
@property (retain) NSTextField *input_field;
@property (retain) NSString *current_model;
@property (retain) NSString *current_query;
@property (assign) double phys_mem_gb;
@property (retain) NSView *first_responder_view;
@property (copy) NSString *launchPath;
@property (retain) NSOutlineView *explorer;
@property (retain) NSString *explorer_dir;
@property (retain) NSTextView *term_out;
@property (retain) NSTextField *term_in;
@property (strong) NSTask *term_task;
@property (retain) NSFileHandle *term_pipe;
@property (retain) NSArray<NSView *> *term_views;
- (void)toggleTerminal;
@property (retain) FileNode *root;
- (void)loadIntoBuffer:(NSString *)path;
- (void)refreshExplorer:(id)sender;
- (FileNode *)loadChildren:(FileNode *)node;
- (void)openNode:(FileNode *)node;
- (void)openFolder:(id)sender;
- (void)startTerminal;
- (void)termRun:(id)sender;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    lvc_buf_init(&_buf, 4096);
    lvc_undo_init(&_undo, 1000);
    lvc_syntax_init(&_syntax);
    lvc_ollama_init(&_ollama, NULL);
    g_ollama = &_ollama;
    if (getenv("LVC_FILE")) {
        strlcpy(g_open_path, getenv("LVC_FILE"), sizeof g_open_path);
    }
    lvc_io_load(&_buf, g_open_path);

    NSRect frame = NSMakeRect(60, 60, 1240, 680);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered
        defer:NO];
    self.window.title = [NSString stringWithFormat:@"LVC — %s", g_open_path];
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.window.backgroundColor = [NSColor colorWithCalibratedWhite:0.035 alpha:1];
    NSView *make_first = nil;
    NSView *content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1240, 680)];
    content.wantsLayer = YES;
    content.layer.backgroundColor = [NSColor colorWithCalibratedWhite:0.02 alpha:1].CGColor;
    const char *renderer = getenv("LVC_RENDERER");
    if (renderer && strcmp(renderer, "metal") == 0) {
        MetalEditorView *mv = [[MetalEditorView alloc]
            initWithBuffer:&_buf syntax:&_syntax undo:&_undo];
        mv.frame = NSMakeRect(220, 210, 680, 470);
        mv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [content addSubview:mv];
        make_first = mv;
    } else {
        self.view = [[EditorView alloc] initWithFrame:NSMakeRect(220, 210, 680, 470)];
        self.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.view.buf = &_buf;
        self.view.undo = &_undo;
        self.view.syntax = &_syntax;
        [content addSubview:self.view];
        make_first = self.view;
    }
    self.first_responder_view = make_first;

    /* --- left: explorer sidebar (file tree + open button) --- */
    unsigned long long mem = 0;
    lvc_sys_phys_mem(&mem);
    _phys_mem_gb = (double)mem / (1024.0 * 1024.0 * 1024.0); /* GiB */

    NSTextField *exlabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 652, 140, 16)];
    exlabel.stringValue = @"EXPLORER";
    exlabel.bordered = NO; exlabel.editable = NO; exlabel.drawsBackground = NO;
    exlabel.textColor = [NSColor colorWithWhite:0.5 alpha:1];
    exlabel.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];

    NSButton *open_btn = [[NSButton alloc] initWithFrame:NSMakeRect(8, 620, 204, 26)];
    open_btn.title = @"📂  파일 열기 (⌘O)";
    open_btn.bezelStyle = NSBezelStyleRounded;
    open_btn.target = self;
    open_btn.action = @selector(openFile:);

    NSButton *open_dir_btn = [[NSButton alloc] initWithFrame:NSMakeRect(8, 592, 204, 26)];
    open_dir_btn.title = @"📁  폴더 열기";
    open_dir_btn.bezelStyle = NSBezelStyleRounded;
    open_dir_btn.target = self;
    open_dir_btn.action = @selector(openFolder:);

    self.explorer = [[NSOutlineView alloc] initWithFrame:NSMakeRect(0, 0, 220, 588)];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"f"];
    col.width = 200;
    col.editable = NO;
    [self.explorer addTableColumn:col];
    self.explorer.outlineTableColumn = col;
    self.explorer.headerView = nil;
    self.explorer.backgroundColor = [NSColor colorWithCalibratedWhite:0.02 alpha:1];
    self.explorer.rowSizeStyle = NSTableViewRowSizeStyleDefault;
    self.explorer.dataSource = self;
    self.explorer.delegate = self;
    self.explorer.indentationPerLevel = 12;
    NSScrollView *esv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 220, 588)];
    esv.documentView = self.explorer;
    esv.hasVerticalScroller = YES;
    esv.backgroundColor = [NSColor colorWithCalibratedWhite:0.02 alpha:1];
    NSView *explorer_panel = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 220, 680)];
    explorer_panel.wantsLayer = YES;
    explorer_panel.layer.backgroundColor = [NSColor colorWithCalibratedWhite:0.015 alpha:1].CGColor;
    [explorer_panel addSubview:exlabel];
    [explorer_panel addSubview:open_btn];
    [explorer_panel addSubview:open_dir_btn];
    [explorer_panel addSubview:esv];
    [content addSubview:explorer_panel];

    /* --- center bottom: terminal panel --- */
    self.term_out = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 680, 158)];
    self.term_out.editable = NO;
    self.term_out.richText = NO;
    self.term_out.backgroundColor = [NSColor colorWithCalibratedWhite:0.01 alpha:1];
    self.term_out.textColor = [NSColor colorWithSRGBRed:0.75 green:0.78 blue:0.75 alpha:1];
    self.term_out.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.term_out.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSScrollView *tsv = [[NSScrollView alloc] initWithFrame:NSMakeRect(224, 34, 676, 158)];
    tsv.documentView = self.term_out;
    tsv.hasVerticalScroller = YES;
    tsv.backgroundColor = [NSColor colorWithCalibratedWhite:0.01 alpha:1];
    tsv.wantsLayer = YES;
    tsv.layer.borderColor = [NSColor colorWithWhite:0.15 alpha:1].CGColor;
    tsv.layer.borderWidth = 1;
    self.term_in = [[NSTextField alloc] initWithFrame:NSMakeRect(224, 6, 676, 24)];
    self.term_in.placeholderString = @"zsh 명령 입력 후 ⏎";
    self.term_in.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.term_in.target = self;
    self.term_in.action = @selector(termRun:);
    NSTextField *tlabel = [[NSTextField alloc] initWithFrame:NSMakeRect(228, 194, 160, 14)];
    tlabel.stringValue = @"TERMINAL — zsh";
    tlabel.bordered = NO; tlabel.editable = NO; tlabel.drawsBackground = NO;
    tlabel.textColor = [NSColor colorWithWhite:0.5 alpha:1];
    tlabel.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
    self.term_views = @[ tsv, self.term_in, tlabel ];
    for (NSView *v in self.term_views) [content addSubview:v];

    /* --- right: AI chat sidebar --- */
    self.sidebar = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 44, 320, 588)];
    self.sidebar.editable = NO;
    self.sidebar.richText = NO;
    self.sidebar.backgroundColor = [NSColor colorWithCalibratedWhite:0.02 alpha:1];
    self.sidebar.textColor = [NSColor colorWithSRGBRed:0.85 green:0.85 blue:0.85 alpha:1];
    self.sidebar.drawsBackground = YES;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(904, 44, 320, 588)];
    sv.documentView = self.sidebar;
    sv.hasVerticalScroller = YES;

    self.model_popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(904, 644, 320, 28)];
    self.input_field = [[NSTextField alloc] initWithFrame:NSMakeRect(904, 8, 220, 28)];
    self.input_field.placeholderString = @"ask about the code… (!task = agent)";
    NSButton *ask = [[NSButton alloc] initWithFrame:NSMakeRect(1128, 8, 96, 28)];
    ask.title = @"Ask";
    ask.bezelStyle = NSBezelStyleRounded;
    ask.target = self;
    ask.action = @selector(askClicked:);

    [content addSubview:sv];
    [content addSubview:self.model_popup];
    [content addSubview:self.input_field];
    [content addSubview:ask];
    self.window.contentView = content;

    [self refreshExplorer:nil];
    [self startTerminal];
    /* defaults BEFORE refreshModels so the fallback check sees them */
    if (!g_model) g_model = @"lvc-coder:latest";
    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(g_fim_log_path, sizeof g_fim_log_path,
                 "%s/.lvc/suggestions.jsonl", home);
    [self refreshModels];
    /* single model serves chat, refactor, and FIM (FIM via raw mode) */
    if (self.launchPath) [self loadIntoBuffer:self.launchPath];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.first_responder_view ?: self.view];

    /* optional clangd: diagnostics + cross-file context; absent is fine */
    g_lsp = malloc(sizeof(lvc_lsp));
    if (g_lsp) {
        char *clangd_argv[] = { (char *)"clangd", NULL };
        char dir[4096], root_uri[4200];
        snprintf(dir, sizeof dir, "%s", g_open_path);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        snprintf(root_uri, sizeof root_uri, "file://%s", dir);
        if (lvc_lsp_start(g_lsp, clangd_argv, root_uri, g_open_path, &_buf) != 0) {
            free(g_lsp);
            g_lsp = NULL;
        } else {
            [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                selector:@selector(pollDiagnostics:) userInfo:nil repeats:YES];
        }
    }
}

- (void)pollDiagnostics:(NSTimer *)t {
    (void)t;
    if (!g_lsp) return;
    if (g_lsp_dirty && g_edit_buf) {
        g_lsp_dirty = NO;
        lvc_lsp_notify_change(g_lsp, g_open_path, g_edit_buf);
    }
    if (!lvc_lsp_poll(g_lsp) || !g_lsp->last_diagnostics) return;
    /* naive scan: newest "message":"..." in the diagnostics payload */
    const char *d = g_lsp->last_diagnostics;
    const char *m = d, *last = NULL;
    while ((m = strstr(m, "\"message\":\"")) != NULL) { last = m; m += 11; }
    if (!last) return;
    last += 11;
    char msg[256];
    size_t w = 0;
    while (*last && *last != '"' && w < sizeof msg - 1) {
        if (*last == '\\' && last[1]) last++;
        msg[w++] = *last++;
    }
    msg[w] = 0;
    static char shown[256] = "";
    if (strcmp(msg, shown) != 0) {
        snprintf(shown, sizeof shown, "%s", msg);
        [self appendChat:[NSString stringWithFormat:@"[clangd] %s\n", msg]];
    }
}

- (void)refreshModels {
    [self.model_popup removeAllItems];
    lvc_model_list list = {0};
    int r = lvc_ollama_list(&_ollama, &list);
    if (r != 0) {
        [self.model_popup addItemWithTitle:@"(ollama offline)"];
        [self appendChat:@"[ollama unreachable — start the Ollama app]\n"];
        return;
    }
    for (size_t i = 0; i < list.count; i++) {
        NSString *title = [NSString stringWithFormat:@"%s (%s)",
            list.items[i].name, list.items[i].size_str];
        [self.model_popup addItemWithTitle:title];
        NSMenuItem *it = [self.model_popup itemAtIndex:(NSInteger)i];
        it.representedObject = [NSString stringWithUTF8String:list.items[i].name];
    }
    /* memory-based recommendation: models under 60% of physical RAM */
    double budget = _phys_mem_gb * 0.6;
    [self appendChat:[NSString stringWithFormat:
        @"[%.0f GB RAM — models up to ~%.0f GB fit comfortably]\n", _phys_mem_gb, budget]];
    /* select g_model in the popup if present, else first item + fallback */
    BOOL found = NO;
    for (size_t i = 0; i < list.count; i++)
        if ([g_model isEqualToString:[NSString stringWithUTF8String:list.items[i].name]])
            { [self.model_popup selectItemAtIndex:(NSInteger)i]; found = YES; break; }
    if (!found && list.count > 0) {
        NSString *old = g_model;
        g_model = [NSString stringWithUTF8String:list.items[0].name];
        [self.model_popup selectItemAtIndex:0];
        [self appendChat:[NSString stringWithFormat:
            @"[model %@ not found — switched to %@]\n", old, g_model]];
    }
    lvc_model_list_free(&list);
}

- (void)appendChat:(NSString *)s {
    [self.sidebar.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:s attributes:@{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11
                weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0.85
                green:0.85 blue:0.85 alpha:1],
        }]];
    [self.sidebar scrollRangeToVisible:NSMakeRange(self.sidebar.string.length, 0)];
}

static void on_stream(const char *chunk, size_t len, void *user) {
    AppDelegate *self = user;
    NSString *s = [[NSString alloc] initWithBytes:chunk length:len
                                         encoding:NSUTF8StringEncoding];
    dispatch_async(dispatch_get_main_queue(), ^{ [self appendChat:s]; });
}

/* ask streaming: sidebar + accumulate for the history file */
typedef struct {
    AppDelegate *del;
    NSMutableData *acc;
} chat_log_ctx;

static void on_ask_stream(const char *chunk, size_t len, void *user) {
    chat_log_ctx *c = user;
    [c->acc appendBytes:chunk length:len];
    NSString *s = [[NSString alloc] initWithBytes:chunk length:len
                                         encoding:NSUTF8StringEncoding];
    dispatch_async(dispatch_get_main_queue(), ^{ [c->del appendChat:s]; });
}

/* append a Q/A pair to $HOME/.lvc/chat_history.txt */
static void append_chat_log(const char *q, const char *a) {
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char dir[512], path[576];
    snprintf(dir, sizeof dir, "%s/.lvc", home);
    snprintf(path, sizeof path, "%s/chat_history.txt", dir);
    mkdir(dir, 0755); /* EEXIST fine */
    FILE *f = fopen(path, "a");
    if (!f) return;
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(f, "==============================\n[%s]\n[질문]\n%s\n\n[답변]\n%s\n"
            "==============================\n\n", ts, q ? q : "", a ? a : "");
    fclose(f);
}

static void *ask_thread(void *arg) {
    AppDelegate *self = arg;
    const char *model_c = g_model.UTF8String;
    const char *q_c = g_query.UTF8String;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self appendChat:[NSString stringWithFormat:@"\nYou: %@\n", g_query]];
    });
    chat_log_ctx cctx = { self, [NSMutableData data] };
    int r = lvc_ollama_generate(&self->_ollama, model_c, q_c, false,
                                on_ask_stream, &cctx);
    NSString *tail = (r == 0) ? @"\n" : [NSString stringWithFormat:@"\n[error %d]\n", r];
    dispatch_async(dispatch_get_main_queue(), ^{ [self appendChat:tail]; });
    if (r == 0) {
        NSString *ans = [[NSString alloc] initWithData:cctx.acc
                                              encoding:NSUTF8StringEncoding];
        append_chat_log(q_c, ans.UTF8String ?: "");
    }
    return NULL;
}

#pragma mark agent mode ("!task" in the chat input)

static void on_agent_event(void *user, lvc_agent_event ev, const char *text) {
    AppDelegate *self = user;
    NSString *s = nil;
    switch (ev) {
    case LVC_AGENT_TOKEN: {
        s = text ? [NSString stringWithUTF8String:text] : nil;
        if (![s length]) return;
        break;
    }
    case LVC_AGENT_TOOL_CALL:
        s = [NSString stringWithFormat:@"\n[agent] ▶ %s\n", text ? text : ""];
        break;
    case LVC_AGENT_TOOL_RESULT: {
        NSString *t = text ? [NSString stringWithUTF8String:text] : @"";
        if (t.length > 400) t = [[t substringToIndex:400] stringByAppendingString:@"…"];
        t = [t stringByReplacingOccurrencesOfString:@"\n" withString:@" ⏎ "];
        s = [NSString stringWithFormat:@"[agent] ◀ %@\n", t];
        break;
    }
    case LVC_AGENT_DONE:
        s = @"\n[agent done]\n";
        break;
    case LVC_AGENT_ERROR:
        s = [NSString stringWithFormat:@"\n[agent error: %s]\n", text ? text : ""];
        break;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ [self appendChat:s]; });
}

static void *agent_thread(void *arg) {
    AppDelegate *self = arg;
    NSString *goal = g_query ?: @"";
    NSString *model = @"qwen2.5-coder:3b"; /* instruct model; FIM model can't chat */
    /* persona (cf. MCF.scpt): concise Korean explanations, clean code */
    NSString *prompt = [NSString stringWithFormat:
        @"다음 작업을 수행하세요. 설명은 간결한 한국어로 하고 코드는 깔끔하게 작성합니다.\n\n목표: %@", goal];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self appendChat:[NSString stringWithFormat:@"\n⟡ agent %@: %@\n", model, goal]];
    });
    const char *host = self->_ollama.base_url;
    if (strncmp(host, "http://", 7) == 0) host += 7;
    char cwd[4096];
    snprintf(cwd, sizeof cwd, "%s", g_open_path);
    char *sl = strrchr(cwd, '/');
    if (sl) *sl = 0; else strcpy(cwd, ".");
    int r = lvc_agent_run(host, model.UTF8String, prompt.UTF8String, cwd,
                          on_agent_event, self);
    if (r == 0) {
        /* agent may have edited files on disk; the open buffer is not reloaded
         * automatically — tell the user instead of clobbering unsaved edits */
        dispatch_async(dispatch_get_main_queue(), ^{
            [self appendChat:
                @"[agent] tip: 파일을 에이전트가 고쳤다면 ⌘O로 다시 열어 동기화하세요\n"];
        });
    }
    return NULL;
}

- (void)askClicked:(id)sender {
    (void)sender;
    NSString *q = nil;
    /* input is a small floating text field appended under the popup */
    q = [self.input_field stringValue];
    NSMenuItem *it = self.model_popup.selectedItem;
    if (!it.representedObject) return;
    if (!q.length) return;
    [self.input_field setStringValue:@""];
    if ([q hasPrefix:@"!"]) { /* agent mode: "!goal" */
        NSString *goal = [q substringFromIndex:1];
        goal = [goal stringByTrimmingCharactersInSet:
            [NSCharacterSet whitespaceCharacterSet]];
        if (!goal.length) return;
        g_query = goal;
        pthread_t t;
        pthread_create(&t, NULL, agent_thread, self);
        pthread_detach(t);
        return;
    }
    g_model = it.representedObject;
    g_query = [q copy];
    pthread_t t;
    pthread_create(&t, NULL, ask_thread, self);
    pthread_detach(t);
}

static void *skill_thread(void *arg) {
    AppDelegate *self = arg;
    const char *model_c = g_model.UTF8String;
    const char *q_c = g_query.UTF8String;
    NSString *header = [g_skill copy];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self appendChat:[NSString stringWithFormat:@"\n[%@]\n", header]];
    });
    int r = lvc_ollama_generate(&self->_ollama, model_c, q_c, false, on_stream, self);
    NSString *tail = (r == 0) ? @"\n" : [NSString stringWithFormat:@"\n[error %d]\n", r];
    dispatch_async(dispatch_get_main_queue(), ^{ [self appendChat:tail]; });
    return NULL;
}

- (void)runSkill:(NSString *)title prompt:(NSString *)prompt {
    if (!g_ollama) return;
    if (prompt.length > 8000) prompt = [prompt substringToIndex:8000];
    g_skill = [title copy];
    g_query = [prompt copy];
    pthread_t t;
    pthread_create(&t, NULL, skill_thread, self);
    pthread_detach(t);
}

- (NSString *)bufferText {
    size_t n = lvc_buf_len(&_buf);
    if (n > 6000) n = 6000;
    char *tmp = malloc(n + 1);
    if (!tmp) return nil;
    lvc_buf_read(&_buf, 0, (uint8_t *)tmp, n);
    tmp[n] = 0;
    NSString *s = [NSString stringWithUTF8String:tmp];
    free(tmp);
    return s ?: @"";
}

- (IBAction)explainFile:(id)sender {
    (void)sender;
    NSString *code = [self bufferText];
    if (!code.length) { [self appendChat:@"[explain: buffer is empty]\n"]; return; }
    [self runSkill:@"explain file"
             prompt:[NSString stringWithFormat:
                @"Explain what this code file does, concisely:\n\n%@\n", code]];
}

- (IBAction)commitMessage:(id)sender {
    (void)sender;
    NSString *code = [self bufferText];
    if (!code.length) { [self appendChat:@"[commit: buffer is empty]\n"]; return; }
    [self runSkill:@"commit message"
             prompt:[NSString stringWithFormat:
                @"Write one conventional-commit message (type(scope): subject) "
                @"for a change producing this file content. Reply with the "
                @"message line only:\n\n%@\n", code]];
}

- (IBAction)explainError:(id)sender {
    (void)sender;
    NSString *term = self.term_out.string ?: @"";
    NSArray *lines = [term componentsSeparatedByString:@"\n"];
    NSUInteger start = lines.count > 40 ? lines.count - 40 : 0;
    NSString *tail = [[lines subarrayWithRange:NSMakeRange(start, lines.count - start)]
        componentsJoinedByString:@"\n"];
    if (!tail.length) { [self appendChat:@"[error: terminal output is empty]\n"]; return; }
    [self runSkill:@"error help"
             prompt:[NSString stringWithFormat:
                @"Explain the cause of this terminal error and how to fix it, "
                @"concisely:\n\n%@\n", tail]];
}

- (void)applicationWillTerminate:(NSNotification *)note {
    (void)note;
    lvc_syntax_free(&_syntax);
    lvc_undo_free(&_undo);
    lvc_buf_free(&_buf);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return YES;
}

- (void)toggleTerminal {
    BOOL hidden = !self.term_views.firstObject.isHidden;
    for (NSView *v in self.term_views) v.hidden = hidden;
    [self appendChat:[NSString stringWithFormat:@"[terminal %@]\n", hidden ? @"hidden" : @"shown"]];
}

- (void)saveFile:(id)sender {
    (void)sender;
    if (lvc_io_save(&_buf, g_open_path) == 0)
        [self appendChat:[NSString stringWithFormat:@"[saved %s]\n", g_open_path]];
    else
        [self appendChat:@"[save failed]\n"];
}

- (void)loadIntoBuffer:(NSString *)path {
    if (lvc_io_load(&_buf, path.UTF8String) != 0) {
        [self appendChat:[NSString stringWithFormat:@"[열기 실패: %@ (텍스트(UTF-8) 파일만 지원)]\n", path.lastPathComponent]];
        return;
    }
    strlcpy(g_open_path, path.UTF8String, sizeof g_open_path);
    self.window.title = [NSString stringWithFormat:@"LVC — %s", g_open_path];
    self.view.cursor = 0;
    lvc_syntax_parse(&_syntax, &_buf);
    [self.view setNeedsDisplay:YES];
    [self appendChat:[NSString stringWithFormat:@"[opened %@]\n", path]];
}

- (void)openFile:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:self.window
        completionHandler:^(NSInteger result) {
            if (result != NSModalResponseOK) return;
            NSString *path = panel.URLs.firstObject.path;
            if (path) [self loadIntoBuffer:path];
        }];
}

- (void)openFolder:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    [panel beginSheetModalForWindow:self.window
        completionHandler:^(NSInteger result) {
            if (result != NSModalResponseOK) return;
            NSString *path = panel.URLs.firstObject.path;
            if (!path) return;
            _root = [FileNode nodeWithPath:path name:path.lastPathComponent isDir:YES];
            [self.explorer reloadData];
            [self appendChat:[NSString stringWithFormat:@"[folder: %@]\n", path]];
        }];
}

- (FileNode *)rootNode {
    if (!_root) {
        char home[1024];
        const char *h = getpwuid(getuid())->pw_dir;
        (void)home;
        NSString *dir = h ? [NSString stringWithUTF8String:h] : @"/";
        _root = [FileNode nodeWithPath:dir name:dir.lastPathComponent ?: dir isDir:YES];
    }
    return _root;
}

- (FileNode *)loadChildren:(FileNode *)node {
    if (node.loaded) return node;
    node.loaded = YES;
    [node.children removeAllObjects];
    DIR *d = opendir(node.path.UTF8String);
    if (!d) return node;
    NSMutableArray<FileNode *> *dirs = [NSMutableArray array];
    NSMutableArray<FileNode *> *files = [NSMutableArray array];
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        NSString *name = [NSString stringWithUTF8String:e->d_name];
        if ([name hasPrefix:@"."]) continue;
        NSString *full = [node.path stringByAppendingPathComponent:name];
        struct stat st;
        if (stat(full.UTF8String, &st) != 0) continue;
        FileNode *child = [FileNode nodeWithPath:full name:name isDir:S_ISDIR(st.st_mode)];
        if (child.isDir) [dirs addObject:child]; else [files addObject:child];
    }
    closedir(d);
    [node.children addObjectsFromArray:dirs];
    [node.children addObjectsFromArray:files];
    return node;
}

- (void)refreshExplorer:(id)sender {
    (void)sender;
    self.root.loaded = NO; /* rescan */
    [self.explorer reloadData];
}

- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    (void)ov;
    FileNode *n = item ?: self.rootNode;
    [self loadChildren:n];
    return (NSInteger)n.children.count;
}

- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)index ofItem:(id)item {
    (void)ov;
    FileNode *n = item ?: self.rootNode;
    return n.children[index];
}

- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item {
    (void)ov;
    return ((FileNode *)item).isDir;
}

- (id)outlineView:(NSOutlineView *)ov objectValueForTableColumn:(NSTableColumn *)c byItem:(id)item {
    (void)ov; (void)c;
    FileNode *n = item;
    return [NSString stringWithFormat:@"%@ %@", n.isDir ? @"📁" : @"📄", n.name];
}

- (void)outlineViewSelectionDidChange:(NSNotification *)note {
    NSOutlineView *ov = note.object;
    FileNode *n = [ov itemAtRow:ov.selectedRow];
    if (n && !n.isDir) [self openNode:n];
}

- (void)openNode:(FileNode *)node {
    struct stat st;
    if (stat(node.path.UTF8String, &st) != 0) return;
    if (node.isDir) { [self.explorer expandItem:node]; return; }
    [self loadIntoBuffer:node.path];
}

#pragma mark terminal

static void term_append(AppDelegate *self, NSString *s) {
    [self.term_out.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:s attributes:@{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0.75 green:0.78 blue:0.75 alpha:1],
        }]];
    [self.term_out scrollRangeToVisible:NSMakeRange(self.term_out.string.length, 0)];
}

- (void)startTerminal {
    self.term_task = [[NSTask alloc] init];
    self.term_task.launchPath = @"/bin/zsh";
    self.term_task.arguments = @[@"-i"];
    NSPipe *out = [NSPipe pipe];
    self.term_task.standardOutput = out;
    self.term_task.standardError = out;
    NSPipe *in = [NSPipe pipe];
    self.term_task.standardInput = in;
    self.term_pipe = in.fileHandleForWriting;
    self.term_task.currentDirectoryPath = self.explorer_dir ?: @"/tmp";
    self.term_task.terminationHandler = ^(NSTask *t) {
        (void)t;
        dispatch_async(dispatch_get_main_queue(), ^{
            term_append(self, @"\n[terminal exited]\n");
        });
    };
    [self.term_task launch];
    out.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData;
        if (!d.length) { h.readabilityHandler = NULL; return; }
        NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]
                      ?: [[NSString alloc] initWithData:d encoding:NSISOLatin1StringEncoding];
        dispatch_async(dispatch_get_main_queue(), ^{ term_append(self, s); });
    };
    term_append(self, @"[zsh ready]\n");
}

- (void)termRun:(id)sender {
    NSString *cmd = self.term_in.stringValue;
    self.term_in.stringValue = @"";
    term_append(self, [NSString stringWithFormat:@"%% %@\n", cmd]);
    [self.term_pipe writeData:[[cmd stringByAppendingString:@"\n"]
        dataUsingEncoding:NSUTF8StringEncoding]];
}
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *del = [AppDelegate new];
        if (argc > 1) del.launchPath = [NSString stringWithUTF8String:argv[1]];
        app.delegate = del;
        app.activationPolicy = NSApplicationActivationPolicyRegular;

        /* menu bar: File > Open (Cmd+O) / Save (Cmd+S) */
        NSMenu *menubar = [[NSMenu alloc] init];
        NSMenuItem *app_item = [[NSMenuItem alloc] init];
        [menubar addItem:app_item];
        NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
        NSMenuItem *file_item = [[NSMenuItem alloc] init];
        file_item.submenu = file;
        [menubar addItem:file_item];
        [file addItemWithTitle:@"Open…" action:@selector(openFile:) keyEquivalent:@"o"];
        [file addItemWithTitle:@"Save" action:@selector(saveFile:) keyEquivalent:@"s"];

        /* AI skills */
        NSMenu *ai = [[NSMenu alloc] initWithTitle:@"AI"];
        NSMenuItem *ai_item = [[NSMenuItem alloc] init];
        ai_item.submenu = ai;
        [menubar addItem:ai_item];
        NSDictionary *skills = @{
            @"Explain File":   @[ @"explainFile:",   @"e" ],
            @"Commit Message": @[ @"commitMessage:",  @"m" ],
            @"Explain Error":  @[ @"explainError:",  @"k" ],
        };
        for (NSString *title in skills) {
            NSArray *cfg = skills[title];
            NSMenuItem *mi = [ai addItemWithTitle:title
                                          action:NSSelectorFromString(cfg[0])
                                  keyEquivalent:cfg[1]];
            mi.keyEquivalentModifierMask =
                NSEventModifierFlagCommand | NSEventModifierFlagOption;
        }
        app.mainMenu = menubar;

        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    (void)argc; (void)argv;
    return 0;
}
