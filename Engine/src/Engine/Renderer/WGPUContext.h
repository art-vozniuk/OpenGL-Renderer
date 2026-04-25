#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>

struct GLFWwindow;

namespace Engine {

	/*
	 * WGPUContext — owns the WebGPU instance, surface, adapter, device,
	 * and queue for the application. There is exactly one of these per
	 * Application; scenes look it up via Application::GetGfx().
	 *
	 * The C-API style WebGPU types (WGPUDevice etc.) are deliberately
	 * exposed raw rather than wrapped in C++ classes — wrapping doesn't
	 * buy us anything for a single-threaded renderer, and downstream code
	 * (compute shaders, custom pipelines) has to drop into the C API
	 * anyway.
	 *
	 * The frame lifecycle is:
	 *     auto frame = ctx.BeginFrame();          // acquire swap-chain view
	 *     if (!frame.valid) return;               // resize / lost; skip frame
	 *     auto pass = ctx.OpenColorPass(frame, clearColor);
	 *     // ... encode draws into `pass` ...
	 *     ctx.ClosePass(pass);
	 *     ctx.EndFrame(frame);                    // submit + present
	 */
	class WGPUContext
	{
	public:
		WGPUContext() = default;
		~WGPUContext();

		WGPUContext(const WGPUContext&) = delete;
		WGPUContext& operator=(const WGPUContext&) = delete;

		// Bring up instance / surface / adapter / device / queue and
		// configure the surface for `width x height`. Returns false on any
		// adapter / device request failure (logs the cause).
		bool Init(GLFWwindow* window, uint32_t width, uint32_t height);
		void Shutdown();

		// Reconfigure swap-chain when the window framebuffer size changes.
		// Called by the resize event handler in Application.
		void OnResize(uint32_t width, uint32_t height);

		WGPUInstance      Instance()      const { return m_Instance; }
		WGPUSurface       Surface()       const { return m_Surface; }
		WGPUAdapter       Adapter()       const { return m_Adapter; }
		WGPUDevice        Device()        const { return m_Device; }
		WGPUQueue         Queue()         const { return m_Queue; }
		WGPUTextureFormat SurfaceFormat() const { return m_SurfaceFormat; }
		uint32_t          Width()         const { return m_Width; }
		uint32_t          Height()        const { return m_Height; }

		// True if the active device has the timestamp-query feature
		// granted. Used by the perf overlay to decide whether to emit
		// GPU-side timings or fall back to CPU encode time only.
		bool              HasTimestampQueries() const { return m_HasTimestampQueries; }

		// Per-frame swap-chain handle. `valid == false` means "skip render
		// this frame" (surface was lost, resize pending, etc.).
		struct Frame
		{
			bool               valid       = false;
			WGPUTexture        surfaceTex  = nullptr;
			WGPUTextureView    view        = nullptr;
			WGPUCommandEncoder encoder     = nullptr;
		};

		Frame BeginFrame();
		void  EndFrame(Frame& frame);

		// Convenience: open a render pass that clears the swap-chain view.
		// The caller still owns the lifetime — call ClosePass() before
		// EndFrame. Optional `timestampWrites` makes the pass record GPU
		// timestamps (used by the gsplat perf overlay); pass nullptr when
		// not measuring.
		WGPURenderPassEncoder OpenColorPass(const Frame& frame,
		                                    float r, float g, float b, float a,
		                                    const WGPUPassTimestampWrites* timestampWrites = nullptr);
		void                  ClosePass(WGPURenderPassEncoder pass);

	private:
		WGPUInstance      m_Instance      = nullptr;
		WGPUSurface       m_Surface       = nullptr;
		WGPUAdapter       m_Adapter       = nullptr;
		WGPUDevice        m_Device        = nullptr;
		WGPUQueue         m_Queue         = nullptr;
		WGPUTextureFormat m_SurfaceFormat = WGPUTextureFormat_Undefined;
		uint32_t          m_Width         = 0;
		uint32_t          m_Height        = 0;
		bool              m_HasTimestampQueries = false;
	};

}
