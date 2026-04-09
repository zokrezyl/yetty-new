// iOS main.mm - Application entry point
//
// Threading model:
// - Main thread: UIKit run loop, creates everything
// - Render via CADisplayLink on main thread (iOS requirement)

#include <yetty/app-context.hpp>
#include <yetty/config.hpp>
#include <yetty/core/platform-input-pipe.hpp>
#include <yetty/core/event.hpp>
#include <yetty/platform/pty-factory.hpp>
#include <yetty/yetty.hpp>
#include <ytrace/ytrace.hpp>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <webgpu/webgpu.h>

std::string getCacheDir();
std::string getRuntimeDir();
WGPUSurface createSurface(WGPUInstance instance, CAMetalLayer* layer);

// =============================================================================
// YettyMetalView - UIView backed by CAMetalLayer
// =============================================================================
@interface YettyMetalView : UIView
@property (nonatomic, readonly) CAMetalLayer* metalLayer;
@end

@implementation YettyMetalView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (CAMetalLayer*)metalLayer {
    return (CAMetalLayer*)self.layer;
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

// =============================================================================
// YettyViewController
// =============================================================================
@interface YettyViewController : UIViewController
@property (nonatomic, strong) YettyMetalView* metalView;
@property (nonatomic, strong) CADisplayLink* displayLink;
@property (nonatomic, assign) yetty::Yetty* yetty;
@property (nonatomic, assign) yetty::core::PlatformInputPipe* pipe;
@property (nonatomic, assign) yetty::Config* config;
@property (nonatomic, assign) yetty::PtyFactory* ptyFactory;
@property (nonatomic, assign) WGPUInstance instance;
@property (nonatomic, assign) WGPUSurface surface;
@end

@implementation YettyViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    self.metalView = [[YettyMetalView alloc] initWithFrame:self.view.bounds];
    self.metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.metalView];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self initializeYetty];
    [self startRenderLoop];
}

- (void)initializeYetty {
    using namespace yetty;

    // 1. Config FIRST
    auto cacheDir = getCacheDir();
    auto runtimeDir = getRuntimeDir();
    PlatformPaths paths = {cacheDir.c_str(), cacheDir.c_str(), runtimeDir.c_str(), nullptr};

    auto configResult = Config::create(0, nullptr, &paths);
    if (!configResult) {
        yerror("Failed to create Config: {}", configResult.error().message());
        return;
    }
    self.config = *configResult;
    ydebug("main: Config created");

    // 2. PlatformInputPipe
    auto pipeResult = core::PlatformInputPipe::create();
    if (!pipeResult) {
        yerror("Failed to create PlatformInputPipe");
        delete self.config;
        return;
    }
    self.pipe = *pipeResult;
    ydebug("main: PlatformInputPipe created");

    // 3. PtyFactory
    auto ptyFactoryResult = PtyFactory::create(self.config);
    if (!ptyFactoryResult) {
        yerror("Failed to create PtyFactory");
        delete self.pipe;
        delete self.config;
        return;
    }
    self.ptyFactory = *ptyFactoryResult;
    ydebug("main: PtyFactory created");

    // 4. WebGPU instance + surface
    self.instance = wgpuCreateInstance(nullptr);
    if (!self.instance) {
        yerror("Failed to create WebGPU instance");
        delete self.ptyFactory;
        delete self.pipe;
        delete self.config;
        return;
    }
    ydebug("main: WebGPU instance created");

    self.surface = createSurface(self.instance, self.metalView.metalLayer);
    if (!self.surface) {
        yerror("Failed to create WebGPU surface");
        wgpuInstanceRelease(self.instance);
        delete self.ptyFactory;
        delete self.pipe;
        delete self.config;
        return;
    }
    ydebug("main: WebGPU surface created");

    // 5. AppContext + Yetty
    AppContext appCtx{};
    appCtx.config = self.config;
    appCtx.platformInputPipe = self.pipe;
    appCtx.ptyFactory = self.ptyFactory;
    appCtx.instance = self.instance;
    appCtx.surface = self.surface;

    auto yettyResult = Yetty::create(appCtx);
    if (!yettyResult) {
        yerror("Failed to create Yetty: {}", yettyResult.error().message());
        wgpuSurfaceRelease(self.surface);
        wgpuInstanceRelease(self.instance);
        delete self.ptyFactory;
        delete self.pipe;
        delete self.config;
        return;
    }
    self.yetty = *yettyResult;
    ydebug("main: Yetty created");
}

- (void)startRenderLoop {
    if (self.displayLink) return;
    self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(renderFrame)];
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSDefaultRunLoopMode];
    ydebug("iOS: Render loop started");
}

- (void)stopRenderLoop {
    [self.displayLink invalidate];
    self.displayLink = nil;
}

- (void)renderFrame {
    if (self.yetty) {
        (void)self.yetty->iterate();
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGSize drawableSize = self.metalView.bounds.size;
    drawableSize.width *= self.metalView.metalLayer.contentsScale;
    drawableSize.height *= self.metalView.metalLayer.contentsScale;
    self.metalView.metalLayer.drawableSize = drawableSize;

    if (self.pipe) {
        auto ev = yetty::core::Event::resizeEvent(
            static_cast<float>(drawableSize.width),
            static_cast<float>(drawableSize.height));
        self.pipe->write(&ev, sizeof(ev));
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    if (!self.pipe) return;

    UITouch* touch = [touches anyObject];
    CGPoint loc = [touch locationInView:self.metalView];
    float scale = self.metalView.metalLayer.contentsScale;

    auto ev = yetty::core::Event{yetty::core::Event::Type::MouseDown};
    ev.mouse = {static_cast<float>(loc.x * scale), static_cast<float>(loc.y * scale), 0, 0};
    self.pipe->write(&ev, sizeof(ev));
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    if (!self.pipe) return;

    UITouch* touch = [touches anyObject];
    CGPoint loc = [touch locationInView:self.metalView];
    float scale = self.metalView.metalLayer.contentsScale;

    auto ev = yetty::core::Event{yetty::core::Event::Type::MouseMove};
    ev.mouseMove = {static_cast<float>(loc.x * scale), static_cast<float>(loc.y * scale), 0};
    self.pipe->write(&ev, sizeof(ev));
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];
    if (!self.pipe) return;

    UITouch* touch = [touches anyObject];
    CGPoint loc = [touch locationInView:self.metalView];
    float scale = self.metalView.metalLayer.contentsScale;

    auto ev = yetty::core::Event{yetty::core::Event::Type::MouseUp};
    ev.mouse = {static_cast<float>(loc.x * scale), static_cast<float>(loc.y * scale), 0, 0};
    self.pipe->write(&ev, sizeof(ev));
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

- (void)dealloc {
    [self stopRenderLoop];
    if (self.yetty) delete self.yetty;
    if (self.surface) wgpuSurfaceRelease(self.surface);
    if (self.instance) wgpuInstanceRelease(self.instance);
    if (self.ptyFactory) delete self.ptyFactory;
    if (self.pipe) delete self.pipe;
    if (self.config) delete self.config;
}

@end

// =============================================================================
// YettyAppDelegate
// =============================================================================
@interface YettyAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow* window;
@property (nonatomic, strong) YettyViewController* viewController;
@end

@implementation YettyAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application;
    (void)launchOptions;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.viewController = [[YettyViewController alloc] init];
    self.window.rootViewController = self.viewController;
    [self.window makeKeyAndVisible];

    ydebug("iOS: App launched");
    return YES;
}

- (void)applicationWillTerminate:(UIApplication *)application {
    (void)application;
    ydebug("iOS: Terminating");
}

@end

// =============================================================================
// main
// =============================================================================
int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([YettyAppDelegate class]));
    }
}
