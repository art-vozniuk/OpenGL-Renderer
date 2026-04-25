#pragma once

#include "Camera.h"
#include "WGPUContext.h"

#include <string>

namespace Engine {

	/*
	 * Renderer — thin per-frame state for scenes.
	 *
	 * The pre-WebGPU version of this class did the OpenGL state machine
	 * dance (RendererAPI / RenderCommand / Submit). All of that lived
	 * upstream of immediate-mode glXxx calls. WebGPU replaces it with
	 * explicit pipelines + command encoders, so the abstraction shrinks
	 * to:
	 *   - hand the per-frame swap-chain handle to the active scene
	 *   - hold the active camera so scenes can grab view/projection
	 *
	 * Scenes that need to draw geometry encode their commands directly
	 * into the active render pass via Renderer::CurrentPass(). The
	 * Application owns the WGPUContext and drives BeginFrame/EndFrame.
	 */
	class Renderer
	{
	public:
		// Called once at app start, after WGPUContext::Init.
		static void Init(WGPUContext* ctx);
		static void Shutdown();

		// Per-frame book-keeping. Two-phase to let the scene encode compute
		// dispatches (e.g. GPU sort) before the render pass is opened:
		//
		//     if (!Renderer::BeginScene(camera)) return;
		//     scene_compute(Renderer::Encoder());          // optional
		//     Renderer::OpenColorPass(r, g, b, a);
		//     scene_render(Renderer::CurrentPass());
		//     Renderer::EndScene();
		//
		// BeginScene returns false if the surface lost the frame (transient
		// resize) — caller should skip render + still call EndScene to
		// release the slot.
		static bool BeginScene(const SPtr<Camera>& camera);
		static void OpenColorPass(float r = 0.05f, float g = 0.05f, float b = 0.08f, float a = 1.0f);
		static void EndScene();

		// Active resources for use inside a BeginScene/EndScene pair.
		static WGPUContext*           Context();
		static WGPUCommandEncoder     Encoder();      // valid between BeginScene and EndScene
		static WGPURenderPassEncoder  CurrentPass();  // valid between OpenColorPass and EndScene
		static const SPtr<Camera>&    GetCamera();

		// Schedule a one-shot framebuffer capture. The next EndScene call
		// will (a) emit a CopyTextureToBuffer of the swap-chain into a
		// staging buffer, (b) submit and present the frame, (c) block on
		// buffer map, (d) write the pixels to `pngPath`, and (e)
		// `std::exit(0)`.
		static void RequestScreenshot(const std::string& pngPath);
	};

}
