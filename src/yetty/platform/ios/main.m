/* iOS main - Application entry point */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <webgpu/webgpu.h>
#include <yetty/yetty.h>
#include <yetty/config.h>
#include <yetty/core/event.h>
#include <yetty/platform/platform-input-pipe.h>
#include <yetty/platform/pty-factory.h>

/* Forward declarations */
const char *yetty_platform_get_cache_dir(void);
const char *yetty_platform_get_runtime_dir(void);
WGPUSurface yetty_platform_create_surface_from_layer(WGPUInstance instance, CAMetalLayer *layer);

/* YettyMetalView - UIView backed by CAMetalLayer */
@interface YettyMetalView : UIView
@property (nonatomic, readonly) CAMetalLayer *metalLayer;
@end

@implementation YettyMetalView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (CAMetalLayer *)metalLayer {
    return (CAMetalLayer *)self.layer;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.metalLayer.device = MTLCreateSystemDefaultDevice();
        self.metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        self.metalLayer.framebufferOnly = YES;
        self.metalLayer.contentsScale = [UIScreen mainScreen].scale;
        self.backgroundColor = [UIColor blackColor];
    }
    return self;
}

@end

/* YettyViewController */
@interface YettyViewController : UIViewController {
    YettyMetalView *_metalView;
    CADisplayLink *_displayLink;
    struct yetty_yetty *_yetty;
    struct yetty_platform_input_pipe *_pipe;
    struct yetty_config *_config;
    struct yetty_platform_pty_factory *_ptyFactory;
    WGPUInstance _instance;
    WGPUSurface _surface;
}
@end

@implementation YettyViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    _metalView = [[YettyMetalView alloc] initWithFrame:self.view.bounds];
    _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_metalView];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self initializeYetty];
    [self startRenderLoop];
}

- (void)initializeYetty {
    const char *cache_dir = yetty_platform_get_cache_dir();
    const char *runtime_dir = yetty_platform_get_runtime_dir();

    struct yetty_platform_paths paths = {
        .shaders_dir = cache_dir,
        .fonts_dir = cache_dir,
        .runtime_dir = runtime_dir,
        .bin_dir = NULL
    };

    /* Config */
    struct yetty_config_result config_result = yetty_config_create(0, NULL, &paths);
    if (!YETTY_IS_OK(config_result)) {
        NSLog(@"Failed to create config");
        return;
    }
    _config = config_result.value;

    /* Platform input pipe */
    struct yetty_platform_input_pipe_result pipe_result = yetty_platform_input_pipe_create();
    if (!YETTY_IS_OK(pipe_result)) {
        NSLog(@"Failed to create input pipe");
        return;
    }
    _pipe = pipe_result.value;

    /* PTY factory */
    struct yetty_platform_pty_factory_result pty_result = yetty_platform_pty_factory_create(_config, NULL);
    if (!YETTY_IS_OK(pty_result)) {
        NSLog(@"Failed to create PTY factory");
        return;
    }
    _ptyFactory = pty_result.value;

    /* WebGPU instance */
    _instance = wgpuCreateInstance(NULL);
    if (!_instance) {
        NSLog(@"Failed to create WebGPU instance");
        return;
    }

    /* Surface */
    _surface = yetty_platform_create_surface_from_layer(_instance, _metalView.metalLayer);
    if (!_surface) {
        NSLog(@"Failed to create surface");
        return;
    }

    /* Create Yetty */
    struct yetty_app_context ctx = {
        .config = _config,
        .platform_input_pipe = _pipe,
        .pty_factory = _ptyFactory,
        .instance = _instance,
        .surface = _surface
    };

    struct yetty_result yetty_result = yetty_create(&ctx);
    if (!YETTY_IS_OK(yetty_result)) {
        NSLog(@"Failed to create Yetty");
        return;
    }
    _yetty = yetty_result.value;
}

- (void)startRenderLoop {
    if (_displayLink) return;
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(renderFrame)];
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSDefaultRunLoopMode];
}

- (void)stopRenderLoop {
    [_displayLink invalidate];
    _displayLink = nil;
}

- (void)renderFrame {
    if (_yetty) {
        yetty_iterate(_yetty);
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGSize size = _metalView.bounds.size;
    CGFloat scale = _metalView.metalLayer.contentsScale;
    size.width *= scale;
    size.height *= scale;
    _metalView.metalLayer.drawableSize = size;

    if (_pipe) {
        struct yetty_core_event ev = yetty_core_event_resize((float)size.width, (float)size.height);
        _pipe->ops->write(_pipe, &ev, sizeof(ev));
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    if (!_pipe) return;

    UITouch *touch = [touches anyObject];
    CGPoint loc = [touch locationInView:_metalView];
    CGFloat scale = _metalView.metalLayer.contentsScale;

    struct yetty_core_event ev = yetty_core_event_mouse_down(
        (float)(loc.x * scale), (float)(loc.y * scale), 0);
    _pipe->ops->write(_pipe, &ev, sizeof(ev));
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    if (!_pipe) return;

    UITouch *touch = [touches anyObject];
    CGPoint loc = [touch locationInView:_metalView];
    CGFloat scale = _metalView.metalLayer.contentsScale;

    struct yetty_core_event ev = yetty_core_event_mouse_move(
        (float)(loc.x * scale), (float)(loc.y * scale));
    _pipe->ops->write(_pipe, &ev, sizeof(ev));
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];
    if (!_pipe) return;

    UITouch *touch = [touches anyObject];
    CGPoint loc = [touch locationInView:_metalView];
    CGFloat scale = _metalView.metalLayer.contentsScale;

    struct yetty_core_event ev = yetty_core_event_mouse_up(
        (float)(loc.x * scale), (float)(loc.y * scale), 0);
    _pipe->ops->write(_pipe, &ev, sizeof(ev));
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

- (void)dealloc {
    [self stopRenderLoop];
    if (_yetty) yetty_destroy(_yetty);
    if (_surface) wgpuSurfaceRelease(_surface);
    if (_instance) wgpuInstanceRelease(_instance);
    if (_ptyFactory) _ptyFactory->ops->destroy(_ptyFactory);
    if (_pipe) _pipe->ops->destroy(_pipe);
    if (_config) yetty_config_destroy(_config);
}

@end

/* YettyAppDelegate */
@interface YettyAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) YettyViewController *viewController;
@end

@implementation YettyAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application;
    (void)launchOptions;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.viewController = [[YettyViewController alloc] init];
    self.window.rootViewController = self.viewController;
    [self.window makeKeyAndVisible];

    return YES;
}

@end

/* main */
int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([YettyAppDelegate class]));
    }
}
