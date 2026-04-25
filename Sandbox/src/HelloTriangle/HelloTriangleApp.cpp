// Stand-alone WebGPU hello-triangle. Lives outside the Engine target so the
// WebGPU toolchain (Dawn fetch, GLFW surface glue, WGSL shader compile) can
// be brought up before we start migrating the rest of the codebase off
// OpenGL. If this binary draws a triangle, the rest of the migration is
// just porting code, not chasing build problems.
//
// Targets the current Dawn / WebGPU C-API (the one with WGPUStringView,
// future-based async, and CallbackInfo-based callback registration). The
// older const-char* / direct-callback flavour shipped with wgpu-native
// pre-v24 doesn't apply here.

#include <webgpu/webgpu.h>
#include <GLFW/glfw3.h>

#ifndef __EMSCRIPTEN__
#  include <glfw3webgpu.h>
#else
#  include <emscripten/emscripten.h>
#  include <emscripten/html5.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>


// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

// Build a WGPUStringView from a NUL-terminated C string. WGPU_STRLEN tells
// the implementation to compute length itself (= strlen).
static inline WGPUStringView SV(const char* s)
{
    WGPUStringView v{};
    v.data   = s;
    v.length = WGPU_STRLEN;
    return v;
}


// ---------------------------------------------------------------------------
//  Shader (WGSL). Single source contains both vertex and fragment entry
//  points; coordinates are hand-set per vertex_index.
// ---------------------------------------------------------------------------
static const char* kShaderWGSL = R"(
struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) color: vec3f,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {
    var positions = array<vec2f, 3>(
        vec2f( 0.0,  0.6),
        vec2f(-0.6, -0.5),
        vec2f( 0.6, -0.5),
    );
    var colors = array<vec3f, 3>(
        vec3f(1.0, 0.2, 0.2),
        vec3f(0.2, 1.0, 0.2),
        vec3f(0.2, 0.4, 1.0),
    );
    var out: VsOut;
    out.pos = vec4f(positions[vid], 0.0, 1.0);
    out.color = colors[vid];
    return out;
}

@fragment
fn fs_main(@location(0) color: vec3f) -> @location(0) vec4f {
    return vec4f(color, 1.0);
}
)";


// ---------------------------------------------------------------------------
//  Globals — single-file demo
// ---------------------------------------------------------------------------
struct App {
    GLFWwindow*        window         = nullptr;
    WGPUInstance       instance       = nullptr;
    WGPUSurface        surface        = nullptr;
    WGPUAdapter        adapter        = nullptr;
    WGPUDevice         device         = nullptr;
    WGPUQueue          queue          = nullptr;
    WGPURenderPipeline pipeline       = nullptr;
    WGPUTextureFormat  surfaceFormat  = WGPUTextureFormat_Undefined;
    int                width          = 1280;
    int                height         = 720;
};

static App g;


// ---------------------------------------------------------------------------
//  Async bootstrap (request adapter + request device)
//
//  Both calls now return a WGPUFuture; we either wait synchronously via
//  wgpuInstanceWaitAny on native, or rely on emscripten's main-loop tick
//  on web. The result lands in a small struct via userdata1.
// ---------------------------------------------------------------------------
struct AdapterRequest { WGPUAdapter handle = nullptr; bool done = false; };
struct DeviceRequest  { WGPUDevice  handle = nullptr; bool done = false; };

static void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                      WGPUStringView msg, void* userdata1, void* /*userdata2*/)
{
    auto* req = static_cast<AdapterRequest*>(userdata1);
    if (status == WGPURequestAdapterStatus_Success) {
        req->handle = adapter;
    } else {
        std::fprintf(stderr, "wgpuRequestAdapter failed: %.*s\n",
                     (int)msg.length, msg.data ? msg.data : "");
    }
    req->done = true;
}

static void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device,
                     WGPUStringView msg, void* userdata1, void* /*userdata2*/)
{
    auto* req = static_cast<DeviceRequest*>(userdata1);
    if (status == WGPURequestDeviceStatus_Success) {
        req->handle = device;
    } else {
        std::fprintf(stderr, "wgpuRequestDevice failed: %.*s\n",
                     (int)msg.length, msg.data ? msg.data : "");
    }
    req->done = true;
}

static void OnDeviceLost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason,
                         WGPUStringView msg, void* /*u1*/, void* /*u2*/)
{
    std::fprintf(stderr, "WGPU device lost (%d): %.*s\n",
                 (int)reason, (int)msg.length, msg.data ? msg.data : "");
}

static void OnUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type,
                              WGPUStringView msg, void* /*u1*/, void* /*u2*/)
{
    std::fprintf(stderr, "WGPU error (%d): %.*s\n",
                 (int)type, (int)msg.length, msg.data ? msg.data : "");
}


// Configure the GLFW-window-backed surface for rendering. Picks the
// preferred surface format and the same logical size as the window.
// Re-called on resize.
static void ConfigureSurface(int w, int h)
{
    WGPUSurfaceCapabilities caps{};
    wgpuSurfaceGetCapabilities(g.surface, g.adapter, &caps);
    g.surfaceFormat = caps.formatCount > 0 ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;

    WGPUSurfaceConfiguration cfg{};
    cfg.device          = g.device;
    cfg.format          = g.surfaceFormat;
    cfg.usage           = WGPUTextureUsage_RenderAttachment;
    cfg.viewFormatCount = 0;
    cfg.viewFormats     = nullptr;
    cfg.alphaMode       = WGPUCompositeAlphaMode_Opaque;
    cfg.width           = (uint32_t)w;
    cfg.height          = (uint32_t)h;
    cfg.presentMode     = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(g.surface, &cfg);
}


static void OnResize(GLFWwindow*, int w, int h)
{
    if (w == 0 || h == 0) return;
    g.width = w;
    g.height = h;
    ConfigureSurface(w, h);
}


// Build the render pipeline that draws the triangle. swapFormat must
// match the surface's current format.
static WGPURenderPipeline CreateTrianglePipeline(WGPUTextureFormat swapFormat)
{
    WGPUShaderSourceWGSL wgsl{};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code        = SV(kShaderWGSL);

    WGPUShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgsl.chain;
    smDesc.label       = SV("triangle.wgsl");

    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(g.device, &smDesc);

    WGPUColorTargetState colorTarget{};
    colorTarget.format    = swapFormat;
    colorTarget.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fs{};
    fs.module      = shader;
    fs.entryPoint  = SV("fs_main");
    fs.targetCount = 1;
    fs.targets     = &colorTarget;

    WGPURenderPipelineDescriptor desc{};
    desc.label                 = SV("triangle");
    desc.vertex.module         = shader;
    desc.vertex.entryPoint     = SV("vs_main");
    desc.primitive.topology    = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.cullMode    = WGPUCullMode_None;
    desc.multisample.count     = 1;
    desc.multisample.mask      = 0xFFFFFFFFu;
    desc.fragment              = &fs;
    desc.layout                = nullptr;  // auto layout — no bindings

    WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(g.device, &desc);
    wgpuShaderModuleRelease(shader);
    return pipe;
}


// One frame: acquire surface texture, encode a render pass that clears +
// draws the triangle, present.
static void RenderFrame()
{
    WGPUSurfaceTexture surfaceTex{};
    wgpuSurfaceGetCurrentTexture(g.surface, &surfaceTex);
    if (surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        ConfigureSurface(g.width, g.height);
        return;
    }

    WGPUTextureViewDescriptor viewDesc{};
    viewDesc.format          = g.surfaceFormat;
    viewDesc.dimension       = WGPUTextureViewDimension_2D;
    viewDesc.mipLevelCount   = 1;
    viewDesc.arrayLayerCount = 1;
    WGPUTextureView view = wgpuTextureCreateView(surfaceTex.texture, &viewDesc);

    WGPUCommandEncoderDescriptor encDesc{};
    encDesc.label = SV("frame");
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, &encDesc);

    WGPURenderPassColorAttachment color{};
    color.view       = view;
    color.loadOp     = WGPULoadOp_Clear;
    color.storeOp    = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{0.06, 0.06, 0.10, 1.0};
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    WGPURenderPassDescriptor rpDesc{};
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments     = &color;

    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rpDesc);
    wgpuRenderPassEncoderSetPipeline(rp, g.pipeline);
    wgpuRenderPassEncoderDraw(rp, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);

    WGPUCommandBufferDescriptor cmdDesc{};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, &cmdDesc);
    wgpuQueueSubmit(g.queue, 1, &cmd);

    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(view);

#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(g.surface);
    wgpuTextureRelease(surfaceTex.texture);
#endif
}


#ifdef __EMSCRIPTEN__
static void EmFrame() { RenderFrame(); glfwPollEvents(); }
#endif


// Drive the WebGPU event loop until `done` becomes true. The default Dawn
// build doesn't expose timed-wait (TimedWaitAny feature unrequested), so
// instead of `wgpuInstanceWaitAny(timeoutNS=N)` we tick events ourselves.
// CallbackMode_AllowProcessEvents arms the callback for these ticks.
static void DrainUntilDone(volatile bool& done)
{
#ifndef __EMSCRIPTEN__
    while (!done) {
        wgpuInstanceProcessEvents(g.instance);
    }
#else
    while (!done) emscripten_sleep(10);
#endif
}


int main()
{
    std::fprintf(stderr, "[HT] main entered\n");
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    std::fprintf(stderr, "[HT] glfw inited\n");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    g.window = glfwCreateWindow(g.width, g.height, "HelloTriangle (WebGPU)", nullptr, nullptr);
    if (!g.window) { std::fprintf(stderr, "glfwCreateWindow failed\n"); return 1; }
    glfwSetFramebufferSizeCallback(g.window, OnResize);

    WGPUInstanceDescriptor instDesc{};
    g.instance = wgpuCreateInstance(&instDesc);
    if (!g.instance) { std::fprintf(stderr, "wgpuCreateInstance failed\n"); return 1; }
    std::fprintf(stderr, "[HT] instance created\n");

#ifndef __EMSCRIPTEN__
    g.surface = glfwCreateWindowWGPUSurface(g.instance, g.window);
#else
    {
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
        canvasDesc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
        canvasDesc.selector    = SV("#canvas");
        WGPUSurfaceDescriptor sDesc{};
        sDesc.nextInChain = &canvasDesc.chain;
        g.surface = wgpuInstanceCreateSurface(g.instance, &sDesc);
    }
#endif
    if (!g.surface) { std::fprintf(stderr, "no surface\n"); return 1; }
    std::fprintf(stderr, "[HT] surface created\n");

    // Adapter
    AdapterRequest aReq;
    WGPURequestAdapterOptions aOpts{};
    aOpts.compatibleSurface = g.surface;
    aOpts.powerPreference   = WGPUPowerPreference_HighPerformance;

    WGPURequestAdapterCallbackInfo aCb{};
    aCb.mode      = WGPUCallbackMode_AllowProcessEvents;
    aCb.callback  = OnAdapter;
    aCb.userdata1 = &aReq;
    (void)wgpuInstanceRequestAdapter(g.instance, &aOpts, aCb);
    DrainUntilDone(aReq.done);
    if (!aReq.handle) { std::fprintf(stderr, "no GPU adapter\n"); return 1; }
    g.adapter = aReq.handle;
    std::fprintf(stderr, "[HT] adapter ready\n");

    // Device
    WGPUDeviceDescriptor dDesc{};
    dDesc.label = SV("main-device");

    WGPUDeviceLostCallbackInfo lostCb{};
    lostCb.mode     = WGPUCallbackMode_AllowProcessEvents;
    lostCb.callback = OnDeviceLost;
    dDesc.deviceLostCallbackInfo = lostCb;

    WGPUUncapturedErrorCallbackInfo errCb{};
    errCb.callback = OnUncapturedError;
    dDesc.uncapturedErrorCallbackInfo = errCb;

    DeviceRequest dReq;
    WGPURequestDeviceCallbackInfo dCb{};
    dCb.mode      = WGPUCallbackMode_AllowProcessEvents;
    dCb.callback  = OnDevice;
    dCb.userdata1 = &dReq;
    (void)wgpuAdapterRequestDevice(g.adapter, &dDesc, dCb);
    DrainUntilDone(dReq.done);
    if (!dReq.handle) { std::fprintf(stderr, "no device\n"); return 1; }
    g.device = dReq.handle;
    g.queue  = wgpuDeviceGetQueue(g.device);
    std::fprintf(stderr, "[HT] device + queue ready\n");

    ConfigureSurface(g.width, g.height);
    std::fprintf(stderr, "[HT] surface configured (format=%d)\n", (int)g.surfaceFormat);
    g.pipeline = CreateTrianglePipeline(g.surfaceFormat);
    std::fprintf(stderr, "[HT] pipeline created, entering main loop\n");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(EmFrame, 0, 1);
#else
    // Headless smoke-test mode: render N frames, exit. Used to verify the
    // toolchain on CI / sleep-less iteration. Without this env var we
    // open the window normally until it's closed.
    int maxFrames = 0;
    if (const char* m = std::getenv("HT_MAX_FRAMES")) maxFrames = std::atoi(m);
    int frame = 0;
    while (!glfwWindowShouldClose(g.window)) {
        glfwPollEvents();
        RenderFrame();
        if (maxFrames > 0 && ++frame >= maxFrames) break;
    }
    std::fprintf(stderr, "rendered %d frames cleanly\n", frame);

    wgpuRenderPipelineRelease(g.pipeline);
    wgpuQueueRelease(g.queue);
    wgpuDeviceRelease(g.device);
    wgpuAdapterRelease(g.adapter);
    wgpuSurfaceRelease(g.surface);
    wgpuInstanceRelease(g.instance);
    glfwDestroyWindow(g.window);
    glfwTerminate();
#endif
    return 0;
}
