#include <nanogui/nanogui.h>
#import <Cocoa/Cocoa.h>

#if defined(NANOGUI_USE_METAL)
#  import <Metal/Metal.h>
#  import <QuartzCore/CAMetalLayer.h>
#  import <QuartzCore/CATransaction.h>
#endif

#if !defined(MAC_OS_X_VERSION_10_15) || \
    MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_15
@interface NSScreen (ForwardDeclare)
@property(readonly)
    CGFloat maximumPotentialExtendedDynamicRangeColorComponentValue;
@end
#endif

NAMESPACE_BEGIN(nanogui)

void chdir_to_bundle_parent() {
    NSString *path = [[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent];
    chdir([path fileSystemRepresentation]);
}

void disable_saved_application_state_osx() {
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"NSQuitAlwaysKeepsWindows"];
}

#if defined(NANOGUI_USE_METAL)

static void *s_metal_device = nullptr;
static void *s_metal_command_queue = nullptr;
static void *s_metal_cleanup_queue = nullptr;

void metal_init() {
    if (s_metal_device || s_metal_command_queue)
        throw std::runtime_error("init_metal(): already initialized!");

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        throw std::runtime_error("init_metal(): unable to create system default device.");

    id<MTLCommandQueue> command_queue = [device newCommandQueue];
    if (!command_queue)
        throw std::runtime_error("init_metal(): unable to create command queue.");

    dispatch_queue_t metal_cleanup_queue = dispatch_queue_create("nanogui.cleanup", DISPATCH_QUEUE_SERIAL);

    s_metal_device = (__bridge_retained void *) device;
    s_metal_command_queue = (__bridge_retained void *) command_queue;
    s_metal_cleanup_queue = (__bridge_retained void *) metal_cleanup_queue;
}

void metal_shutdown() {
    if (s_metal_cleanup_queue) {
        dispatch_queue_t queue = (__bridge dispatch_queue_t) s_metal_cleanup_queue;
        dispatch_sync(queue, ^{});  // Wait for pending work
        (void) (__bridge_transfer dispatch_queue_t) s_metal_cleanup_queue;  // Transfer to ARC for release
        s_metal_cleanup_queue = nullptr;
    }

    if (s_metal_device) {
        (void) (__bridge_transfer id<MTLDevice>) s_metal_device;
        s_metal_device = nullptr;
    }

    if (s_metal_command_queue) {
        (void) (__bridge_transfer id<MTLCommandQueue>) s_metal_command_queue;
        s_metal_command_queue = nullptr;
    }
}

void* metal_device() { return s_metal_device; }
void* metal_command_queue() { return s_metal_command_queue; }
void* metal_cleanup_queue() { return s_metal_cleanup_queue; }

void metal_sync() {
    id<MTLCommandQueue> command_queue = (__bridge id<MTLCommandQueue>) s_metal_command_queue;
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
}

void metal_window_init(void *nswin_, bool float_buffer) {
    CAMetalLayer *layer = [CAMetalLayer layer];
    if (!layer)
        throw std::runtime_error("init_metal(): unable to create layer.");
    [layer setOpaque: YES];
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    nswin.contentView.wantsLayer = YES;
    nswin.contentView.layer = layer;
    nswin.contentView.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
    layer.device = (__bridge id<MTLDevice>) s_metal_device;
    layer.contentsScale = nswin.backingScaleFactor;
    if (float_buffer) {
        layer.wantsExtendedDynamicRangeContent = YES;
        layer.pixelFormat = MTLPixelFormatRGBA16Float;
        layer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB);
    } else {
        layer.wantsExtendedDynamicRangeContent = NO;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    }
    layer.maximumDrawableCount = 2;
    layer.allowsNextDrawableTimeout = NO;
    layer.framebufferOnly = YES;
    layer.presentsWithTransaction = NO;
}

void metal_window_set_vsync(void *nswin_, bool vsync) {
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    CAMetalLayer *layer = (CAMetalLayer *) nswin.contentView.layer;
    layer.displaySyncEnabled = vsync ? YES : NO;
}

std::pair<bool, bool> metal_10bit_edr_support() {
    NSArray<NSScreen *> * screens = [NSScreen screens];
    bool buffer_10bit = false,
         buffer_ext = false;

    for (NSScreen * screen in screens) {
        if ([screen canRepresentDisplayGamut: NSDisplayGamutP3])
            buffer_10bit = true; // on macOS, P3 gamut is equivalent to 10 bit color depth

        if ([screen respondsToSelector:@selector
                      (maximumPotentialExtendedDynamicRangeColorComponentValue)]) {
            if ([screen maximumPotentialExtendedDynamicRangeColorComponentValue] >= 2.f) {
                buffer_10bit = true;
                buffer_ext = true;
            }
        }
    }

    return { buffer_10bit, buffer_ext };
}

void* metal_layer(void *nswin_) {
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    CAMetalLayer *layer = (CAMetalLayer *) nswin.contentView.layer;
    return (__bridge void *) layer;
}


void metal_window_set_size(void *nswin_, const Vector2i &size) {
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    CAMetalLayer *layer = (CAMetalLayer *) nswin.contentView.layer;
    layer.drawableSize = CGSizeMake(size.x(), size.y());
}

void metal_window_set_content_scale(void *nswin_, float scale) {
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    CAMetalLayer *layer = (CAMetalLayer *) nswin.contentView.layer;

    float old_scale = layer.contentsScale;
    if (old_scale != scale) {
        [CATransaction begin];
        [CATransaction setValue:(id)kCFBooleanTrue
                        forKey:kCATransactionDisableActions];
        layer.contentsScale = scale;
        [CATransaction commit];
    }
}

void* metal_window_layer(void *nswin_) {
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    return (__bridge void *) nswin.contentView.layer;
}

void metal_window_next_drawable(void *nswin_, void **drawable_, void **texture_) {
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    CAMetalLayer *layer = (CAMetalLayer *) nswin.contentView.layer;
    id<CAMetalDrawable> drawable = layer.nextDrawable;
    *texture_ = (__bridge void *) drawable.texture;
    *drawable_ = (__bridge_retained void *) drawable;
}

void metal_window_set_framebuffer_only(void *nswin_, bool value) {
    NSWindow *nswin = (__bridge NSWindow *) nswin_;
    CAMetalLayer *layer = (CAMetalLayer *) nswin.contentView.layer;
    layer.framebufferOnly = value ? YES : NO;
}

bool metal_read_texture_rgba8(void *texture_, std::vector<uint8_t> &rgba, Vector2i &size) {
    id<MTLTexture> texture = (__bridge id<MTLTexture>) texture_;
    if (!texture || texture.framebufferOnly)
        return false;
    const MTLPixelFormat format = texture.pixelFormat;
    const bool half = format == MTLPixelFormatRGBA16Float;
    const bool bgra = format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB;
    if (!half && !bgra)
        return false;

    const NSUInteger width = texture.width, height = texture.height;
    const NSUInteger bytes_per_row = width * (half ? 8 : 4);
    id<MTLDevice> device = (__bridge id<MTLDevice>) s_metal_device;
    id<MTLCommandQueue> command_queue = (__bridge id<MTLCommandQueue>) s_metal_command_queue;
    id<MTLBuffer> buffer = [device newBufferWithLength: bytes_per_row * height
                                               options: MTLResourceStorageModeShared];
    if (!buffer)
        return false;
    // Same queue as the frame itself, so the copy runs after everything drawn so far.
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    [blit copyFromTexture: texture
              sourceSlice: 0
              sourceLevel: 0
             sourceOrigin: MTLOriginMake(0, 0, 0)
               sourceSize: MTLSizeMake(width, height, 1)
                 toBuffer: buffer
        destinationOffset: 0
   destinationBytesPerRow: bytes_per_row
 destinationBytesPerImage: bytes_per_row * height];
    [blit endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    size = Vector2i((int) width, (int) height);
    rgba.resize((size_t) width * height * 4);
    const uint8_t *src = (const uint8_t *) buffer.contents;
    for (size_t i = 0; i < (size_t) width * height; ++i) {
        if (half) {
            // Extended sRGB: values above 1 (HDR highlights) are clipped for the picture.
            const __fp16 *p = (const __fp16 *) (src + i * 8);
            for (int c = 0; c < 3; ++c) {
                float v = (float) p[c];
                v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                rgba[i * 4 + c] = (uint8_t) (v * 255.f + 0.5f);
            }
        } else {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
        }
        rgba[i * 4 + 3] = 255;
    }
    return true;
}

void metal_present_and_release_drawable(void *drawable_) {
    id<CAMetalDrawable> drawable = (__bridge_transfer id<CAMetalDrawable>) drawable_;
    id<MTLCommandQueue> command_queue = (__bridge id<MTLCommandQueue>) s_metal_command_queue;
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    [command_buffer presentDrawable: drawable];
    [command_buffer commit];
}

#endif

NAMESPACE_END(nanogui)
