/* iOS main - Application entry point */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <webgpu/webgpu.h>
#include <pthread.h>
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

/* Render thread args */
struct render_thread_args {
    struct yetty_yetty *yetty;
    int *running;
};

static void *render_thread_func(void *arg)
{
    struct render_thread_args *args = arg;
    yetty_run(args->yetty);
    *(args->running) = 0;
    return NULL;
}

/* YettyViewController */
@interface YettyViewController : UIViewController {
    YettyMetalView *_metalView;
    struct yetty_yetty *_yetty;
    struct yetty_platform_input_pipe *_pipe;
    struct yetty_config *_config;
    struct yetty_platform_pty_factory *_ptyFactory;
    WGPUInstance _instance;
    WGPUSurface _surface;
    pthread_t _renderThread;
    int _running;
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

    /* Get framebuffer size */
    CGSize size = _metalView.bounds.size;
    CGFloat scale = _metalView.metalLayer.contentsScale;
    uint32_t fb_width = (uint32_t)(size.width * scale);
    uint32_t fb_height = (uint32_t)(size.height * scale);
    _metalView.metalLayer.drawableSize = CGSizeMake(fb_width, fb_height);

    /* Create Yetty */
    struct yetty_app_context ctx = {
        .app_gpu_context = {
            .instance = _instance,
            .surface = _surface,
            .surface_width = fb_width,
            .surface_height = fb_height
        },
        .config = _config,
        .platform_input_pipe = _pipe,
        .clipboard_manager = NULL,
        .pty_factory = _ptyFactory
    };

    struct yetty_yetty_result yetty_result = yetty_create(&ctx);
    if (!YETTY_IS_OK(yetty_result)) {
        NSLog(@"Failed to create Yetty");
        return;
    }
    _yetty = yetty_result.value;

    /* Start render thread */
    _running = 1;
    struct render_thread_args *args = malloc(sizeof(struct render_thread_args));
    args->yetty = _yetty;
    args->running = &_running;
    pthread_create(&_renderThread, NULL, render_thread_func, args);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGSize size = _metalView.bounds.size;
    CGFloat scale = _metalView.metalLayer.contentsScale;
    size.width *= scale;
    size.height *= scale;
    _metalView.metalLayer.drawableSize = size;

    if (_pipe) {
        struct yetty_core_event ev = {0};
        ev.type = YETTY_EVENT_RESIZE;
        ev.resize.width = (float)size.width;
        ev.resize.height = (float)size.height;
        _pipe->ops->write(_pipe, &ev, sizeof(ev));
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    if (!_pipe) return;

    UITouch *touch = [touches anyObject];
    CGPoint loc = [touch locationInView:_metalView];
    CGFloat scale = _metalView.metalLayer.contentsScale;

    struct yetty_core_event ev = {0};
    ev.type = YETTY_EVENT_MOUSE_DOWN;
    ev.mouse.x = (float)(loc.x * scale);
    ev.mouse.y = (float)(loc.y * scale);
    ev.mouse.button = 0;
    ev.mouse.mods = 0;
    _pipe->ops->write(_pipe, &ev, sizeof(ev));
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    if (!_pipe) return;

    UITouch *touch = [touches anyObject];
    CGPoint loc = [touch locationInView:_metalView];
    CGFloat scale = _metalView.metalLayer.contentsScale;

    struct yetty_core_event ev = {0};
    ev.type = YETTY_EVENT_MOUSE_MOVE;
    ev.mouse.x = (float)(loc.x * scale);
    ev.mouse.y = (float)(loc.y * scale);
    _pipe->ops->write(_pipe, &ev, sizeof(ev));
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];
    if (!_pipe) return;

    UITouch *touch = [touches anyObject];
    CGPoint loc = [touch locationInView:_metalView];
    CGFloat scale = _metalView.metalLayer.contentsScale;

    struct yetty_core_event ev = {0};
    ev.type = YETTY_EVENT_MOUSE_UP;
    ev.mouse.x = (float)(loc.x * scale);
    ev.mouse.y = (float)(loc.y * scale);
    ev.mouse.button = 0;
    ev.mouse.mods = 0;
    _pipe->ops->write(_pipe, &ev, sizeof(ev));
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

- (void)dealloc {
    _running = 0;
    if (_renderThread) {
        pthread_join(_renderThread, NULL);
    }
    if (_yetty) yetty_destroy(_yetty);
    if (_surface) wgpuSurfaceRelease(_surface);
    if (_instance) wgpuInstanceRelease(_instance);
    if (_ptyFactory) _ptyFactory->ops->destroy(_ptyFactory);
    if (_pipe) _pipe->ops->destroy(_pipe);
    if (_config) _config->ops->destroy(_config);
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
