/* Metal-backed text renderer: glyph atlas (ASCII via CoreText rasterization)
 * + per-frame instanced quads. Text/colors come from the C core buffer. */
#import <MetalKit/MetalKit.h>
#include "../core/buffer.h"
#include "../core/syntax.h"
#include "../core/undo.h"

@interface MetalEditorView : MTKView
@property (assign) lvc_buf *buf;
@property (assign) lvc_syntax *syntax;
@property (assign) lvc_undo *undo;
@property (assign) size_t cursor;
@property (assign) NSString *ghost;
@property (assign) float line_height;
@property (assign) float cell_width;
- (id)initWithBuffer:(lvc_buf *)buf syntax:(lvc_syntax *)syntax undo:(lvc_undo *)undo;
- (void)rebuildAtlas;
@end
