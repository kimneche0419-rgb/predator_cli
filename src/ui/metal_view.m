#import "metal_view.h"
#import <CoreText/CoreText.h>
#include <string.h>
#include <stdlib.h>
#include "../core/io.h"

static void lvc_io_save_extern(lvc_buf *b) {
    extern const char *lvc_scratch_path(void);
    lvc_io_save(b, lvc_scratch_path());
}

static const char *SHADER = R"(
#include <metal_stdlib>
using namespace metal;
struct VIn {
    float2 pos   [[attribute(0)]];
    float2 uv    [[attribute(1)]];
    float4 color [[attribute(2)]];
};
struct VOut {
    float4 pos [[position]];
    float2 uv;
    float4 color;
};
vertex VOut vmain(const VIn in [[stage_in]],
                  constant float2 &viewport [[buffer(1)]]) {
    VOut o;
    o.pos = float4(in.pos.x / viewport.x * 2.0 - 1.0,
                   1.0 - in.pos.y / viewport.y * 2.0, 0, 1);
    o.uv = in.uv;
    o.color = in.color;
    return o;
}
fragment float4 fmain(VOut in [[stage_in]],
                      texture2d<float> atlas [[texture(0)]]) {
    constexpr sampler s(address::clamp_to_edge);
    /* color.a >= 1 means solid block (cursor), else modulate by glyph alpha */
    float solid = saturate(in.color.a - 0.5);
    float a = atlas.sample(s, in.uv).r;
    return float4(in.color.rgb, mix(a, 1.0, solid) * min(in.color.a, 1.0));
}
)";

typedef struct { float x, y, u, v, r, g, b, a; } mvtx;

typedef struct { float v[4]; } float4; /* host-side color (shader mirrors with simd) */

@implementation MetalEditorView {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipe;
    id<MTLTexture> _atlas;
    id<MTLBuffer> _vbuf;
    mvtx *_verts;
    size_t _vert_cap, _vert_len;
    int _glyph_w, _glyph_h;
    float4 _plain;
}

- (id)initWithBuffer:(lvc_buf *)buf syntax:(lvc_syntax *)syntax undo:(lvc_undo *)undo {
    _device = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:CGRectMake(0, 0, 600, 600) device:_device];
    if (!self) return nil;
    _buf = buf; _syntax = syntax; _undo = undo;
    self.enableSetNeedsDisplay = YES;
    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.clearColor = MTLClearColorMake(0.08, 0.08, 0.10, 1);
    _queue = [_device newCommandQueue];

    NSError *err = nil;
    id<MTLLibrary> lib = [_device newLibraryWithSource:@(SHADER) options:nil error:&err];
    if (!lib) { NSLog(@"shader: %@", err.localizedDescription); return nil; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
    vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8;  vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format = MTLVertexFormatFloat4; vd.attributes[2].offset = 16; vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride = 32;
    pd.vertexDescriptor = vd;
    _pipe = [_device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!_pipe) { NSLog(@"pipe: %@", err.localizedDescription); return nil; }

    _glyph_w = 10; _glyph_h = 18;
    _line_height = _glyph_h + 4;
    self.cell_width = _glyph_w;
    _plain = (float4){{0.85f, 0.85f, 0.85f, 1.0f}};
    _vert_cap = 1 << 20;
    _verts = malloc(_vert_cap * sizeof(mvtx));
    _vbuf = [_device newBufferWithLength:_vert_cap * sizeof(mvtx)
                                 options:MTLResourceStorageModeShared];
    [self rebuildAtlas];
    self.delegate = (id<MTKViewDelegate>)self;
    return self;
}

- (void)dealloc { free(_verts); }

- (BOOL)acceptsFirstResponder { return YES; }

- (void)insertBytes:(const uint8_t *)bytes len:(size_t)len {
    uint32_t start = (uint32_t)self.cursor;
    ptrdiff_t r = lvc_buf_insert(self.buf, self.cursor, bytes, len);
    if (r > 0) {
        lvc_undo_push(self.undo, LVC_OP_INSERT, self.cursor, bytes, len, self.undo->next_group);
        self.cursor += (size_t)r;
        lvc_syntax_edit(self.syntax, self.buf, start, start, (uint32_t)self.cursor);
    }
}

- (void)deleteRange:(size_t)pos len:(size_t)n {
    uint32_t start = (uint32_t)pos;
    ptrdiff_t r = lvc_undo_delete(self.undo, self.buf, pos, n, self.undo->next_group);
    if (r > 0) lvc_syntax_edit(self.syntax, self.buf, start, (uint32_t)(pos + n), start);
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = event.characters;
    if (!chars.length) return;
    unichar c = [chars characterAtIndex:0];
    NSUInteger flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (flags & NSEventModifierFlagCommand) {
        if (c == 'z') {
            if (flags & NSEventModifierFlagShift) lvc_undo_redo(self.undo, self.buf);
            else lvc_undo_undo(self.undo, self.buf);
        } else if (c == 's') {
            lvc_io_save_extern(self.buf);
        }
        return;
    }
    switch (c) {
    case NSDeleteCharacter: {
        if (self.cursor > 0) {
            size_t prev = self.cursor - 1;
            while (prev > 0 && (lvc_buf_get(self.buf, prev) & 0xC0) == 0x80) prev--;
            [self deleteRange:prev len:self.cursor - prev];
        }
        return;
    }
    case '\r': [self insertBytes:(const uint8_t *)"\n" len:1]; return;
    case '\t': [self insertBytes:(const uint8_t *)"    " len:4]; return;
    default: break;
    }
    if (c >= 0xF700) return;
    const char *utf8 = chars.UTF8String;
    [self insertBytes:(const uint8_t *)utf8 len:strlen(utf8)];
}

/* rasterize ASCII 32..126 into atlas via CoreText, one glyph per cell */
- (void)rebuildAtlas {
    const int first = 32, last = 126;
    const int n = last - first + 1, per_row = 32;
    const int rows = (n + per_row - 1) / per_row;
    const int W = per_row * _glyph_w, H = rows * _glyph_h;

    uint8_t *px = calloc(1, (size_t)W * H);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(px, W, H, 8, W, cs, kCGImageAlphaNone);
    CGColorSpaceRelease(cs);
    CGContextSetFillColorWithColor(ctx, CGColorGetConstantColor(kCGColorWhite));
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);

    CTFontRef font = CTFontCreateWithName(CFSTR("Menlo"), 13.0, NULL);
    CGContextSetFont(ctx, CTFontCopyGraphicsFont(font, NULL));
    CGContextSetFontSize(ctx, 13.0);
    for (int i = 0; i < n; i++) {
        UniChar ch = (UniChar)(first + i);
        CGGlyph g;
        if (!CTFontGetGlyphsForCharacters(font, &ch, &g, 1)) continue;
        int cx = (i % per_row) * _glyph_w;
        int cy = (i / per_row) * _glyph_h;
        /* context y-up over top-down memory: memory_row = H - context_y.
         * baseline at memory cy+15 keeps glyph inside its cell (asc≈12.5). */
        CGPoint p = CGPointMake(cx + 1, H - cy - 15);
        CGContextShowGlyphsAtPositions(ctx, &g, &p, 1);
    }
    CFRelease(font);

    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
        width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    _atlas = [_device newTextureWithDescriptor:td];
    [_atlas replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0
                withBytes:px bytesPerRow:W];
    CGContextRelease(ctx);
    free(px);
}

static void push_quad(mvtx **verts, size_t *len, size_t cap,
                      float x, float y, int ci,
                      int gw, int gh, int tex_w, int tex_h, float4 c) {
    if (*len + 6 > cap) return;
    float u0 = (float)((ci % 32) * gw) / tex_w;
    float u1 = u0 + (float)gw / tex_w;
    float v0 = (float)((ci / 32) * gh) / tex_h;   /* cell top */
    float v1 = v0 + (float)gh / tex_h;            /* cell bottom */
    mvtx q[6] = {
        {x,           y,        u0, v0, c.v[0], c.v[1], c.v[2], c.v[3]},
        {x + gw,      y,        u1, v0, c.v[0], c.v[1], c.v[2], c.v[3]},
        {x,           y + gh,   u0, v1, c.v[0], c.v[1], c.v[2], c.v[3]},
        {x + gw,      y,        u1, v0, c.v[0], c.v[1], c.v[2], c.v[3]},
        {x + gw,      y + gh,   u1, v1, c.v[0], c.v[1], c.v[2], c.v[3]},
        {x,           y + gh,   u0, v1, c.v[0], c.v[1], c.v[2], c.v[3]},
    };
    memcpy(*verts + *len, q, sizeof q);
    *len += 6;
}

static float4 color_for_kind(lvc_token_kind k) {
    switch (k) {
    case LVC_TK_KEYWORD: return (float4){{0.80f, 0.45f, 0.85f, 1}};
    case LVC_TK_STRING:  return (float4){{0.70f, 0.85f, 0.45f, 1}};
    case LVC_TK_COMMENT: return (float4){{0.45f, 0.50f, 0.45f, 1}};
    case LVC_TK_NUMBER:  return (float4){{0.85f, 0.65f, 0.40f, 1}};
    case LVC_TK_TYPE:    return (float4){{0.45f, 0.75f, 0.85f, 1}};
    default:             return (float4){{0.85f, 0.85f, 0.85f, 1}};
    }
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (void)drawInMTKView:(MTKView *)view {
    CGSize vs = view.drawableSize;
    _vert_len = 0;

    lvc_span spans[512];
    size_t nspans = lvc_syntax_spans(self.syntax, spans, 512);
    size_t span_i = 0;

    size_t n = lvc_buf_len(self.buf);
    size_t line_start = 0;
    int row = 0;
    while (line_start <= n && (float)(row + 1) * self.line_height < vs.height) {
        char chunk[4096];
        size_t take = 0, p = line_start;
        while (p < n && take < sizeof chunk - 1) {
            uint8_t c = lvc_buf_get(self.buf, p);
            chunk[take++] = (char)c; p++;
            if (c == '\n') break;
        }
        chunk[take] = 0;
        float y = 8 + (float)row * self.line_height;
        for (size_t k = 0; k < take; k++) {
            int idx = (int)(unsigned char)chunk[k] - 32;
            if (idx < 0 || idx > 94) continue;
            while (span_i + 1 < nspans && spans[span_i].end <= line_start + k) span_i++;
            float4 c = (span_i < nspans && spans[span_i].start <= line_start + k &&
                        line_start + k < spans[span_i].end)
                ? color_for_kind(spans[span_i].kind) : _plain;
            push_quad(&_verts, &_vert_len, _vert_cap,
                      10 + (float)k * self.cell_width, y, idx,
                      _glyph_w, _glyph_h, (int)_atlas.width, (int)_atlas.height, c);
        }
        row++;
        if (p == line_start) break;
        line_start = p;
    }

    /* cursor: solid block at current line start (line tracking approximate) */
    size_t cur_line = 0;
    for (size_t i = 0; i < self.cursor && i < n; i++)
        if (lvc_buf_get(self.buf, i) == '\n') cur_line++;
    float4 cc = {1, 1, 1, 2.0f}; /* a >= 1 -> solid block */
    push_quad(&_verts, &_vert_len, _vert_cap,
              10, 8 + (float)cur_line * self.line_height, 0,
              3, _glyph_h, (int)_atlas.width, (int)_atlas.height, cc);

    memcpy(_vbuf.contents, _verts, _vert_len * sizeof(mvtx));

    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    MTLRenderPassDescriptor *rp = view.currentRenderPassDescriptor;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:_pipe];
    [enc setVertexBuffer:_vbuf offset:0 atIndex:0];
    float vp[2] = { (float)vs.width, (float)vs.height };
    [enc setVertexBytes:vp length:sizeof vp atIndex:1];
    [enc setFragmentTexture:_atlas atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:_vert_len];
    [enc endEncoding];
    [cb presentDrawable:view.currentDrawable];
    [cb commit];
}

@end
