#include "GaussianSplatScene.h"
#include "SceneRegistry.h"
#include "../SceneSelector.h"

#include "Engine/Application.h"
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SplatLoader.h"

#include <GLFW/glfw3.h>
#include <filesystem>
#include <vector>
#include <cstring>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/fetch.h>
#endif

using namespace Engine;

namespace Sandbox {

	namespace {

		// Default spawn used when neither query param nor CLI arg is provided.
		// Tuned for the antimatter15 train.splat — for arbitrary scenes the
		// caller is expected to pass good eye/fwd, otherwise the splats may
		// end up off-screen until the user flies the camera around.
		struct GsplatSceneSpawn {
			glm::vec3 eye;
			glm::vec3 fwd;
		};

		static const GsplatSceneSpawn kDefaultSpawn = {
			/*eye=*/ glm::vec3(-4.60f,  0.70f,  4.30f),
			/*fwd=*/ glm::vec3( 0.49f, -0.14f, -0.86f),
		};


	#ifdef __EMSCRIPTEN__

		// State shared between the fetch callbacks and the calling code.
		struct FetchState {
			bool                  done   = false;
			int                   status = 0;
			std::vector<uint8_t>  data;
		};

		void PostSplatMessage(const char* json) {
			// Forward a JSON string to the parent frame. Wrapped in a
			// try/catch since postMessage throws if the parent is gone.
			EM_ASM({
				try {
					if (typeof window !== 'undefined' && window.parent !== window) {
						window.parent.postMessage(JSON.parse(UTF8ToString($0)), '*');
					}
				} catch (e) {}
			}, json);
		}

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
			PostSplatMessage(buf);
		}

		// Async fetch with ASYNCIFY-style busy wait: spin emscripten_sleep
		// while the JS event loop downloads the blob. Fires `splat-progress`
		// messages to the parent frame as bytes come in. Synchronous from
		// the caller's perspective — returns parsed SplatData (empty on
		// failure).
		SplatData FetchSplatViaXHR(const std::string& url) {
			FetchState state;

			emscripten_fetch_attr_t attr;
			emscripten_fetch_attr_init(&attr);
			std::strcpy(attr.requestMethod, "GET");
			attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
			attr.onsuccess  = OnFetchSuccess;
			attr.onerror    = OnFetchError;
			attr.onprogress = OnFetchProgress;
			attr.userData   = &state;

			INFO_CORE("GaussianSplatScene: fetching '{0}'", url);
			emscripten_fetch(&attr, url.c_str());

			while (!state.done) {
				emscripten_sleep(16);
			}

			if (state.status != 200) {
				ERROR_CORE("GaussianSplatScene: fetch failed for {0}: HTTP {1}",
				           url, state.status);
				return {};
			}

			// Bytes are in — switch the parent's loader UI to "decoding".
			PostSplatMessage("{\"type\":\"splat-decoding\"}");

			return SplatLoader::LoadSplatFromBytes(
				state.data.data(), state.data.size(), url.c_str());
		}

	#endif // __EMSCRIPTEN__


		// Resolve the scene source path / URL from runtime params, falling
		// back to the bundled train.splat asset for native dev when nothing
		// was passed.
		std::string ResolveSceneSource() {
		#ifdef __EMSCRIPTEN__
			if (auto u = ReadParam("scene_url"); u) return *u;
		#endif
			if (auto p = ReadParam("scene_path"); p) return *p;
			// Native fallback for local dev: keep loading the bundled train
			// scene. Once the file is removed from the repo the caller MUST
			// pass --scene_path=<file>.
			namespace fs = std::filesystem;
			return (fs::path(ENGINE_ASSETS_DIR) / "splat" / "train.splat").string();
		}

	} // namespace


	GaussianSplatScene::GaussianSplatScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat", screenWidth, screenHeight)
	{
		m_Camera.SetPerspective(glm::radians(45.0f),
		                        m_ScreenWidth / m_ScreenHeight,
		                        0.1f, 10000.0f);

		const std::string source = ResolveSceneSource();

		SplatData data;
	#ifdef __EMSCRIPTEN__
		// Web: scene_url is an absolute https URL; fall through to file
		// load if someone passes a non-URL (debug only).
		if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
			data = FetchSplatViaXHR(source);
		} else {
			data = SplatLoader::LoadSplat(source);
		}
	#else
		data = SplatLoader::LoadSplat(source);
	#endif

		if (data.Empty()) {
			ERROR_CORE("GaussianSplatScene: failed to load '{0}'", source);
			return;
		}
		m_SplatCount = data.Count();

		// Camera spawn: prefer query/CLI params, otherwise use defaults.
		// ParseVec3 returns nullopt on bad input — fall through.
		glm::vec3 eye = kDefaultSpawn.eye;
		glm::vec3 fwd = glm::normalize(kDefaultSpawn.fwd);
		if (auto s = ReadParam("eye"); s) {
			if (auto v = ParseVec3(*s); v) eye = *v;
		}
		if (auto s = ReadParam("fwd"); s) {
			if (auto v = ParseVec3(*s); v) fwd = glm::normalize(*v);
		}

		INFO_CORE("gsplat spawn: eye=({0},{1},{2}) fwd=({3},{4},{5})",
		          eye.x, eye.y, eye.z, fwd.x, fwd.y, fwd.z);
		m_Camera.SetTransform(glm::inverse(glm::lookAt(eye, eye + fwd, glm::vec3(0.0f, 1.0f, 0.0f))));
		m_Camera.m_MoveSpeed = 1.0f;

		m_Splats = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		m_Splats->Upload(data);

	#ifdef __EMSCRIPTEN__
		// Tell the parent the splat is uploaded and we're about to start
		// drawing — wrapper hides the loading bar on this signal.
		PostSplatMessage("{\"type\":\"splat-ready\"}");
	#endif
	}


	void GaussianSplatScene::OnUpdate(Timestep ts)
	{
		m_Camera.Update(ts);

		// Frame-interval sample (CPU wallclock between scene tick starts).
		// Captured before any encoding work so it reflects the user-visible
		// rate, not just our sort/render budget.
		const double frameStart = glfwGetTime();
		if (m_Splats && m_PrevFrameStart > 0.0) {
			m_Splats->Metrics().frameMs.Push(
				static_cast<float>((frameStart - m_PrevFrameStart) * 1000.0));
		}
		m_PrevFrameStart = frameStart;

		// Drain async timestamp readbacks from previous frames into the
		// metrics ring before we schedule the next round.
		if (m_Splats) m_Splats->TickPerf();

		if (!Renderer::BeginScene(m_Camera.GetRenderCamera())) {
			return;
		}

		// GPU sort runs every frame, BEFORE the colour pass is opened
		// (compute and render passes can't share an encoder once a render
		// pass is active). The renderer reads only `sortedIndices` so
		// re-sorting is just one buffer rewrite, not the bulk reshuffle
		// the GL renderer used to do.
		const double encodeStart = glfwGetTime();
		const glm::mat4& view = m_Camera.GetRenderCamera()->GetViewMatrix();
		if (m_Splats) m_Splats->EncodeSort(Renderer::Encoder(), view);

		const WGPUPassTimestampWrites* renderTw =
			m_Splats ? m_Splats->GetRenderPassTimestampWrites() : nullptr;
		Renderer::OpenColorPass(0.05f, 0.05f, 0.08f, 1.0f, renderTw);
		if (m_Splats) {
			m_Splats->EncodeRender(Renderer::CurrentPass(),
			                       m_Camera.GetRenderCamera(),
			                       glm::vec2(m_ScreenWidth, m_ScreenHeight));
		}

		// The render pass writes its end-of-pass timestamp on close, so we
		// have to close it BEFORE running ResolveAndReadTimestamps —
		// otherwise the encoder is still locked by the open pass and the
		// resolve call errors out.
		Renderer::ClosePass();

		// Resolve the four timestamp queries into the next ring slot. Has
		// to happen while the encoder is still open and BEFORE the swap
		// (Renderer::EndScene closes the pass and submits).
		if (m_Splats) m_Splats->ResolveAndReadTimestamps(Renderer::Encoder());

		const double encodeEnd = glfwGetTime();
		if (m_Splats) {
			m_Splats->Metrics().cpuEncodeMs.Push(
				static_cast<float>((encodeEnd - encodeStart) * 1000.0));
			m_Splats->Metrics().splatCount = static_cast<int>(m_SplatCount);
			m_Splats->Metrics().Emit();
		}

		// Roll a 60-frame FPS counter and print every 60 frames so we can
		// see steady-state perf without the load-time outlier on frame 0.
		++m_FpsCounter;
		double now = glfwGetTime();
		if (m_FpsT0 == 0.0) m_FpsT0 = now;
		if (m_FpsCounter % 60 == 0) {
			float dt = (float)(now - m_FpsT0);
			INFO_CORE("gsplat: 60 frames in {0:.3f}s = {1:.1f} fps", dt, 60.0f / dt);
			m_FpsT0 = now;

			// Periodic camera-pose dump so an interactive native session can
			// fly to a pleasing angle and read off the eye/fwd values for
			// the DB seed (camera_eye / camera_fwd columns).
			const glm::mat4& t = m_Camera.GetTransform();
			const glm::vec3 e = glm::vec3(t[3]);
			const glm::vec3 f = -glm::vec3(t[2]);
			INFO_CORE("gsplat camera: eye=({0:.3f},{1:.3f},{2:.3f}) fwd=({3:.3f},{4:.3f},{5:.3f})",
			          e.x, e.y, e.z, f.x, f.y, f.z);
		}

		// Headless screenshot hook (single-shot capture + exit).
		// GS_CAPTURE_FRAME (default 30) chooses which frame to grab —
		// useful for capturing multiple frames in succession to diff
		// inter-frame stability when chasing flicker.
		++m_FrameCount;
		int captureAt = 30;
		if (const char* f = std::getenv("GS_CAPTURE_FRAME")) captureAt = std::atoi(f);
		if (m_FrameCount == captureAt) {
			if (const char* p = std::getenv("GS_CAPTURE_PATH")) {
				if (*p) Renderer::RequestScreenshot(p);
			}
		}

		Renderer::EndScene();
	}

	SCENE_REGISTER("gsplat", GaussianSplatScene)

}
