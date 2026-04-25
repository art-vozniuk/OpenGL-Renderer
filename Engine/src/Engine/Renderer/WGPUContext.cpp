#include "pch.h"
#include "WGPUContext.h"

#include "Engine/Log.h"

#include <GLFW/glfw3.h>
#ifndef __EMSCRIPTEN__
#  include <glfw3webgpu.h>
#else
#  include <emscripten/emscripten.h>
#endif

namespace Engine {

	namespace {

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

		struct AdapterRequest { WGPUAdapter handle = nullptr; bool done = false; };
		struct DeviceRequest  { WGPUDevice  handle = nullptr; bool done = false; };

		void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
		               WGPUStringView msg, void* userdata1, void*)
		{
			auto* req = static_cast<AdapterRequest*>(userdata1);
			if (status == WGPURequestAdapterStatus_Success) {
				req->handle = adapter;
			} else {
				ERROR_CORE("wgpuRequestAdapter failed: {0}", std::string(msg.data ? msg.data : "", msg.length));
			}
			req->done = true;
		}

		void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device,
		              WGPUStringView msg, void* userdata1, void*)
		{
			auto* req = static_cast<DeviceRequest*>(userdata1);
			if (status == WGPURequestDeviceStatus_Success) {
				req->handle = device;
			} else {
				ERROR_CORE("wgpuRequestDevice failed: {0}", std::string(msg.data ? msg.data : "", msg.length));
			}
			req->done = true;
		}

		void OnDeviceLost(WGPUDevice const*, WGPUDeviceLostReason reason,
		                  WGPUStringView msg, void*, void*)
		{
			// Reason `Destroyed` fires on normal teardown — stay quiet there.
			if (reason == WGPUDeviceLostReason_Destroyed) return;
			ERROR_CORE("WGPU device lost ({0}): {1}", (int)reason,
			           std::string(msg.data ? msg.data : "", msg.length));
		}

		void OnUncapturedError(WGPUDevice const*, WGPUErrorType type,
		                       WGPUStringView msg, void*, void*)
		{
			ERROR_CORE("WGPU error ({0}): {1}", (int)type,
			           std::string(msg.data ? msg.data : "", msg.length));
		}

	} // namespace


	WGPUContext::~WGPUContext()
	{
		Shutdown();
	}


	bool WGPUContext::Init(GLFWwindow* window, uint32_t width, uint32_t height)
	{
		m_Width  = width;
		m_Height = height;

		WGPUInstanceDescriptor instDesc{};
		m_Instance = wgpuCreateInstance(&instDesc);
		if (!m_Instance) { ERROR_CORE("wgpuCreateInstance failed"); return false; }

#ifndef __EMSCRIPTEN__
		m_Surface = glfwCreateWindowWGPUSurface(m_Instance, window);
#else
		WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
		canvasDesc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
		canvasDesc.selector    = SV("#canvas");
		WGPUSurfaceDescriptor sDesc{};
		sDesc.nextInChain = &canvasDesc.chain;
		m_Surface = wgpuInstanceCreateSurface(m_Instance, &sDesc);
#endif
		if (!m_Surface) { ERROR_CORE("wgpu surface create failed"); return false; }
		INFO_CORE("WGPU surface ready, requesting adapter...");

		// Adapter (async with poll-based wait)
		AdapterRequest aReq;
		WGPURequestAdapterOptions aOpts{};
		aOpts.compatibleSurface = m_Surface;
		aOpts.powerPreference   = WGPUPowerPreference_HighPerformance;
		WGPURequestAdapterCallbackInfo aCb{};
		// Spontaneous fires whenever — on emscripten that's "right after the
		// JS callback resolves, on the next event-loop tick we yield to via
		// emscripten_sleep". On native it fires from inside the next
		// wgpuInstanceProcessEvents call.
		aCb.mode      = WGPUCallbackMode_AllowSpontaneous;
		aCb.callback  = OnAdapter;
		aCb.userdata1 = &aReq;
		(void)wgpuInstanceRequestAdapter(m_Instance, &aOpts, aCb);
		while (!aReq.done) {
#ifdef __EMSCRIPTEN__
			// Browser drives the event loop itself; ASYNCIFY makes this
			// look synchronous in C++ while yielding to the JS scheduler.
			emscripten_sleep(10);
#else
			wgpuInstanceProcessEvents(m_Instance);
#endif
		}
		if (!aReq.handle) { ERROR_CORE("adapter request returned null"); return false; }
		m_Adapter = aReq.handle;
		INFO_CORE("WGPU adapter ready, requesting device...");

		// Device (request `TimedWaitAny` so future-based polls work without
		// the constant `Timeout waits not enabled` spam).
		WGPUDeviceDescriptor dDesc{};
		dDesc.label = SV("engine-device");

		// Optional features. timestamp-query is only present on a subset
		// of (browser × OS × driver) combos — we ask for it but tolerate
		// failure. The renderer's perf overlay falls back to CPU-only
		// timings when this isn't granted.
		WGPUFeatureName optionalFeatures[1] = { WGPUFeatureName_TimestampQuery };
		const bool adapterHasTimestamp =
			wgpuAdapterHasFeature(m_Adapter, WGPUFeatureName_TimestampQuery) != 0;
		if (adapterHasTimestamp) {
			dDesc.requiredFeatures      = optionalFeatures;
			dDesc.requiredFeatureCount  = 1;
		}
		m_HasTimestampQueries = adapterHasTimestamp;
		INFO_CORE("WGPU timestamp-query feature: {0}",
		          adapterHasTimestamp ? "available" : "not available");

		WGPUDeviceLostCallbackInfo lostCb{};
		lostCb.mode     = WGPUCallbackMode_AllowProcessEvents;
		lostCb.callback = OnDeviceLost;
		dDesc.deviceLostCallbackInfo = lostCb;

		WGPUUncapturedErrorCallbackInfo errCb{};
		errCb.callback = OnUncapturedError;
		dDesc.uncapturedErrorCallbackInfo = errCb;

		DeviceRequest dReq;
		WGPURequestDeviceCallbackInfo dCb{};
		dCb.mode      = WGPUCallbackMode_AllowSpontaneous;
		dCb.callback  = OnDevice;
		dCb.userdata1 = &dReq;
		(void)wgpuAdapterRequestDevice(m_Adapter, &dDesc, dCb);
		while (!dReq.done) {
#ifdef __EMSCRIPTEN__
			emscripten_sleep(10);
#else
			wgpuInstanceProcessEvents(m_Instance);
#endif
		}
		if (!dReq.handle) return false;
		m_Device = dReq.handle;
		m_Queue  = wgpuDeviceGetQueue(m_Device);

		// Configure swap chain.
		WGPUSurfaceCapabilities caps{};
		wgpuSurfaceGetCapabilities(m_Surface, m_Adapter, &caps);
		m_SurfaceFormat = caps.formatCount > 0 ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;

		WGPUSurfaceConfiguration cfg{};
		cfg.device      = m_Device;
		cfg.format      = m_SurfaceFormat;
		// CopySrc lets us read the surface texture back into a CPU buffer
		// for screenshots / regression captures. Negligible cost when
		// nothing maps, real value when iterating on the renderer.
		cfg.usage       = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
		cfg.alphaMode   = WGPUCompositeAlphaMode_Opaque;
		cfg.width       = m_Width;
		cfg.height      = m_Height;
		cfg.presentMode = WGPUPresentMode_Fifo;
		wgpuSurfaceConfigure(m_Surface, &cfg);

		INFO_CORE("WGPUContext initialised ({0}x{1}, format={2})",
		          m_Width, m_Height, (int)m_SurfaceFormat);
		return true;
	}


	void WGPUContext::Shutdown()
	{
		if (m_Queue)    { wgpuQueueRelease(m_Queue);   m_Queue   = nullptr; }
		if (m_Device)   { wgpuDeviceRelease(m_Device); m_Device  = nullptr; }
		if (m_Adapter)  { wgpuAdapterRelease(m_Adapter); m_Adapter = nullptr; }
		if (m_Surface)  { wgpuSurfaceRelease(m_Surface); m_Surface = nullptr; }
		if (m_Instance) { wgpuInstanceRelease(m_Instance); m_Instance = nullptr; }
	}


	void WGPUContext::OnResize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0) return;
		m_Width = width; m_Height = height;
		if (!m_Device) return;

		WGPUSurfaceConfiguration cfg{};
		cfg.device      = m_Device;
		cfg.format      = m_SurfaceFormat;
		// CopySrc lets us read the surface texture back into a CPU buffer
		// for screenshots / regression captures. Negligible cost when
		// nothing maps, real value when iterating on the renderer.
		cfg.usage       = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
		cfg.alphaMode   = WGPUCompositeAlphaMode_Opaque;
		cfg.width       = m_Width;
		cfg.height      = m_Height;
		cfg.presentMode = WGPUPresentMode_Fifo;
		wgpuSurfaceConfigure(m_Surface, &cfg);
	}


	WGPUContext::Frame WGPUContext::BeginFrame()
	{
		Frame f{};

		WGPUSurfaceTexture tex{};
		wgpuSurfaceGetCurrentTexture(m_Surface, &tex);
		if (tex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
		    tex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
			// Reconfigure & try next frame; drop this one.
			OnResize(m_Width, m_Height);
			return f;
		}
		f.surfaceTex = tex.texture;

		WGPUTextureViewDescriptor viewDesc{};
		viewDesc.format          = m_SurfaceFormat;
		viewDesc.dimension       = WGPUTextureViewDimension_2D;
		viewDesc.mipLevelCount   = 1;
		viewDesc.arrayLayerCount = 1;
		f.view = wgpuTextureCreateView(f.surfaceTex, &viewDesc);

		WGPUCommandEncoderDescriptor encDesc{};
		encDesc.label = SV("frame-encoder");
		f.encoder = wgpuDeviceCreateCommandEncoder(m_Device, &encDesc);

		f.valid = true;
		return f;
	}


	void WGPUContext::EndFrame(Frame& frame)
	{
		if (!frame.valid) return;

		WGPUCommandBufferDescriptor cmdDesc{};
		WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(frame.encoder, &cmdDesc);
		wgpuQueueSubmit(m_Queue, 1, &cmd);

		wgpuCommandBufferRelease(cmd);
		wgpuCommandEncoderRelease(frame.encoder);
		wgpuTextureViewRelease(frame.view);

#ifndef __EMSCRIPTEN__
		wgpuSurfacePresent(m_Surface);
		wgpuTextureRelease(frame.surfaceTex);
#endif

		frame = Frame{};

		// Drive callback queue between frames so async ops (mapping, etc.)
		// progress.
		wgpuInstanceProcessEvents(m_Instance);
	}


	WGPURenderPassEncoder WGPUContext::OpenColorPass(const Frame& frame,
	                                                  float r, float g, float b, float a,
	                                                  const WGPUPassTimestampWrites* timestampWrites)
	{
		WGPURenderPassColorAttachment color{};
		color.view       = frame.view;
		color.loadOp     = WGPULoadOp_Clear;
		color.storeOp    = WGPUStoreOp_Store;
		color.clearValue = WGPUColor{r, g, b, a};
		color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

		WGPURenderPassDescriptor rpDesc{};
		rpDesc.label                = SV("color-pass");
		rpDesc.colorAttachmentCount = 1;
		rpDesc.colorAttachments     = &color;
		// Optional: have the pass write timestamps to a query set on
		// begin / end. Used by the perf overlay; nullptr when timestamp
		// queries aren't available on this device.
		rpDesc.timestampWrites      = timestampWrites;

		return wgpuCommandEncoderBeginRenderPass(frame.encoder, &rpDesc);
	}


	void WGPUContext::ClosePass(WGPURenderPassEncoder pass)
	{
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}

}
