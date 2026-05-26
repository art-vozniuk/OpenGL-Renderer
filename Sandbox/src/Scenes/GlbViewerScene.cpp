#include "GlbViewerScene.h"

#include "SceneRegistry.h"
#include "../SceneSelector.h"

#include "Engine/Application.h"
#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/Log.h"
#include "Engine/Renderer/GltfLoader.h"
#include "Engine/Renderer/Renderer.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/fetch.h>
#endif

using namespace Engine;

namespace Sandbox {

	namespace {

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

	#ifdef __EMSCRIPTEN__

		struct FetchState {
			bool                  done   = false;
			int                   status = 0;
			std::vector<uint8_t>  data;
		};

		void OnFetchSuccess(emscripten_fetch_t* f) {
			auto* s = static_cast<FetchState*>(f->userData);
			s->status = f->status;
			if (f->numBytes > 0 && f->data) {
				s->data.assign(reinterpret_cast<const uint8_t*>(f->data),
				               reinterpret_cast<const uint8_t*>(f->data) + f->numBytes);
			}
			s->done = true;
			emscripten_fetch_close(f);
		}

		void OnFetchError(emscripten_fetch_t* f) {
			auto* s = static_cast<FetchState*>(f->userData);
			s->status = f->status;
			s->done = true;
			emscripten_fetch_close(f);
		}

		void OnFetchProgress(emscripten_fetch_t* f) {
			if (f->totalBytes == 0) return;
			char buf[160];
			std::snprintf(buf, sizeof(buf),
			              "{\"type\":\"splat-progress\",\"loaded\":%llu,\"total\":%llu}",
			              (unsigned long long)f->dataOffset,
			              (unsigned long long)f->totalBytes);
			SceneBase::PostSceneMessage(buf);
		}

		// Mirrors GaussianSplatScene::FetchSplatViaXHR — keep the two
		// independent (single network helper would be ~5 shared lines).
		std::vector<uint8_t> FetchBytesViaXHR(const std::string& url) {
			FetchState state;

			emscripten_fetch_attr_t attr;
			emscripten_fetch_attr_init(&attr);
			std::strcpy(attr.requestMethod, "GET");
			attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
			attr.onsuccess  = OnFetchSuccess;
			attr.onerror    = OnFetchError;
			attr.onprogress = OnFetchProgress;
			attr.userData   = &state;

			INFO_CORE("GlbViewerScene: fetching '{0}'", url);
			emscripten_fetch(&attr, url.c_str());

			while (!state.done) {
				emscripten_sleep(16);
			}

			if (state.status != 200) {
				ERROR_CORE("GlbViewerScene: fetch failed for {0}: HTTP {1}",
				           url, state.status);
				return {};
			}
			return std::move(state.data);
		}

	#endif // __EMSCRIPTEN__


		std::vector<uint8_t> ReadFileBytes(const std::string& path) {
			std::ifstream f(path, std::ios::binary);
			if (!f) return {};
			f.seekg(0, std::ios::end);
			const auto sz = f.tellg();
			f.seekg(0, std::ios::beg);
			std::vector<uint8_t> out((size_t)sz);
			if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
			return out;
		}


		std::string ResolveGlbSource() {
		#ifdef __EMSCRIPTEN__
			if (auto u = ReadParam("scene_url"); u) return *u;
		#endif
			if (auto p = ReadParam("scene_path"); p) return *p;
			return {};
		}

	} // namespace


	GlbViewerScene::GlbViewerScene(float screenWidth, float screenHeight)
		: SceneBase("glb_viewer", screenWidth, screenHeight)
	{
		// LMB camera (viewer convention).
		m_CameraConfig.dragButton = 0; // MOUSE_BUTTON_LEFT

		const std::string source = ResolveGlbSource();
		if (source.empty()) {
			ERROR_CORE("GlbViewerScene: ?scene_url= (or --scene_path=) is required");
			SwitchCameraToOrbit(glm::vec3(0.0f), glm::vec3(0.0f, 0.5f, 2.5f));
			return;
		}

		std::vector<uint8_t> bytes;
	#ifdef __EMSCRIPTEN__
		if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
			bytes = FetchBytesViaXHR(source);
		} else {
			bytes = ReadFileBytes(source);
		}
	#else
		bytes = ReadFileBytes(source);
	#endif

	#ifdef __EMSCRIPTEN__
		PostSceneMessage("{\"type\":\"splat-decoding\"}");
	#endif

		MeshData data = GltfLoader::LoadGlbFromBytes(bytes.data(), bytes.size(),
		                                             source.c_str());
		if (data.Empty()) {
			ERROR_CORE("GlbViewerScene: failed to parse glb '{0}'", source);
			SwitchCameraToOrbit(glm::vec3(0.0f), glm::vec3(0.0f, 0.5f, 2.5f));
			return;
		}

		// Auto-frame: target = AABB center; eye pulled back along +Z and
		// raised slightly so the mesh isn't dead-on. Magic numbers tuned to
		// roughly match the SHARP viewer's framing on a unit-ish subject.
		glm::vec3 target(0.0f, 0.5f, 0.0f);
		glm::vec3 eye(0.0f, 0.7f, 2.5f);
		if (data.aabbValid) {
			const glm::vec3 ext    = data.aabbMax - data.aabbMin;
			const float     radius = std::max(0.1f, 0.5f * glm::length(ext));
			target = 0.5f * (data.aabbMin + data.aabbMax);
			eye    = target + glm::vec3(0.0f, radius * 0.4f, radius * 2.4f);
		}
		INFO_CORE("glb_viewer spawn: target=({0:.2f},{1:.2f},{2:.2f}) eye=({3:.2f},{4:.2f},{5:.2f})",
		          target.x, target.y, target.z, eye.x, eye.y, eye.z);
		SwitchCameraToOrbit(target, eye);

		m_Mesh = std::make_unique<MeshRenderer>(Application::Get().GetGfx());
		m_Mesh->Upload(data);
		m_Mesh->SetModelMatrix(glm::mat4(1.0f));

	#ifdef __EMSCRIPTEN__
		// React's SplatViewer listens for 'splat-ready' to hide the loading
		// overlay — same signal works for any scene kind.
		PostSceneMessage("{\"type\":\"splat-ready\"}");
	#endif
	}


	GlbViewerScene::~GlbViewerScene()
	{
		if (m_DepthView) { wgpuTextureViewRelease(m_DepthView); m_DepthView = nullptr; }
		if (m_DepthTex)  { wgpuTextureRelease(m_DepthTex);      m_DepthTex  = nullptr; }
	}


	void GlbViewerScene::EnsureDepthTexture(uint32_t w, uint32_t h)
	{
		if (w == 0 || h == 0) return;
		if (m_DepthTex && m_DepthWidth == w && m_DepthHeight == h) return;
		if (m_DepthView) { wgpuTextureViewRelease(m_DepthView); m_DepthView = nullptr; }
		if (m_DepthTex)  { wgpuTextureRelease(m_DepthTex);      m_DepthTex  = nullptr; }

		WGPUContext& ctx = Application::Get().GetGfx();
		WGPUTextureDescriptor td{};
		td.label = SV("glb-viewer-depth");
		td.usage = WGPUTextureUsage_RenderAttachment;
		td.dimension = WGPUTextureDimension_2D;
		td.size.width  = w;
		td.size.height = h;
		td.size.depthOrArrayLayers = 1;
		td.format = MeshRenderer::DepthFormat();
		td.mipLevelCount = 1;
		td.sampleCount   = 1;
		m_DepthTex = wgpuDeviceCreateTexture(ctx.Device(), &td);

		WGPUTextureViewDescriptor vd{};
		vd.format = td.format;
		vd.dimension = WGPUTextureViewDimension_2D;
		vd.aspect = WGPUTextureAspect_DepthOnly;
		vd.mipLevelCount = 1;
		vd.arrayLayerCount = 1;
		m_DepthView = wgpuTextureCreateView(m_DepthTex, &vd);

		m_DepthWidth = w;
		m_DepthHeight = h;
	}


	void GlbViewerScene::OnUpdate(Timestep ts)
	{
		HandleStandardCameraArbitration(/*autoFlipOnFlyKey=*/true);

		// Tab snaps fly → orbit, mirroring GaussianSplatPlayerScene.
		const bool tabDown = Input::IsKeyPressed(KEY_TAB);
		if (tabDown && !m_PrevTabDown && m_Camera->Mode() == CameraMode::Fly) {
			const PoseSnapshot s = m_Camera->Snapshot();
			SwitchCameraToOrbit(s.orbitTarget, s.position);
		}
		m_PrevTabDown = tabDown;

		DrainUnusedOrbitInput();
		m_Camera->Update(ts);

		const SPtr<Camera> activeCam = m_Camera->GetRenderCamera();

		if (!Renderer::BeginScene(activeCam)) return;

		if (!m_Mesh) {
			// Nothing loaded — still open a clear pass so we don't show
			// a stale framebuffer.
			Renderer::OpenColorPass(0.05f, 0.05f, 0.08f, 1.0f);
			Renderer::ClosePass();
			Renderer::EndScene();
			return;
		}

		WGPUContext& ctx = Application::Get().GetGfx();
		EnsureDepthTexture(ctx.Width(), ctx.Height());

		WGPUTextureView frameView = Renderer::FrameView();
		if (frameView && m_DepthView) {
			WGPURenderPassColorAttachment color{};
			color.view       = frameView;
			color.loadOp     = WGPULoadOp_Clear;
			color.storeOp    = WGPUStoreOp_Store;
			color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			color.clearValue = WGPUColor{ 0.05, 0.05, 0.08, 1.0 };

			WGPURenderPassDepthStencilAttachment depth{};
			depth.view            = m_DepthView;
			depth.depthLoadOp     = WGPULoadOp_Clear;
			depth.depthStoreOp    = WGPUStoreOp_Store;
			depth.depthClearValue = 1.0f;
			depth.depthReadOnly   = 0;
			depth.stencilLoadOp   = WGPULoadOp_Undefined;
			depth.stencilStoreOp  = WGPUStoreOp_Undefined;
			depth.stencilReadOnly = 1;

			WGPURenderPassDescriptor rp{};
			rp.label                   = SV("glb-viewer-pass");
			rp.colorAttachmentCount    = 1;
			rp.colorAttachments        = &color;
			rp.depthStencilAttachment  = &depth;

			WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
				Renderer::Encoder(), &rp);

			const glm::vec3 lDir(0.4f, 0.8f, 0.4f);   // direction TO light
			const glm::vec3 lColor(1.0f, 0.97f, 0.93f);
			const glm::vec3 ambient(0.18f, 0.19f, 0.22f);
			m_Mesh->EncodeRender(pass, activeCam,
			                     glm::vec2((float)ctx.Width(), (float)ctx.Height()),
			                     lDir, lColor, ambient);

			wgpuRenderPassEncoderEnd(pass);
			wgpuRenderPassEncoderRelease(pass);
		}

		Renderer::EndScene();
		(void)ts;
	}


	SCENE_REGISTER("glb_viewer", GlbViewerScene)

}
