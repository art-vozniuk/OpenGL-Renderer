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

		// Async fetch with ASYNCIFY-style busy wait: spin emscripten_sleep
		// while the JS event loop downloads the blob.
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

			SceneBase::PostSceneMessage("{\"type\":\"splat-decoding\"}");

			return SplatLoader::LoadSplatFromBytes(
				state.data.data(), state.data.size(), url.c_str());
		}

	#endif // __EMSCRIPTEN__


		std::string ResolveSceneSource() {
		#ifdef __EMSCRIPTEN__
			if (auto u = ReadParam("scene_url"); u) return *u;
		#endif
			if (auto p = ReadParam("scene_path"); p) return *p;
			namespace fs = std::filesystem;
			return (fs::path(ENGINE_ASSETS_DIR) / "splat" / "train.splat").string();
		}

	} // namespace


	GaussianSplatScene::GaussianSplatScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat", screenWidth, screenHeight)
	{
		// LMB camera (viewer convention). SceneBase reads this when
		// constructing any camera through SwitchCameraTo*.
		m_CameraConfig.dragButton = 0; // MOUSE_BUTTON_LEFT

		const std::string source = ResolveSceneSource();

		SplatData data;
	#ifdef __EMSCRIPTEN__
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
			// Still install a default orbit camera so the empty scene renders.
			SwitchCameraToOrbit(glm::vec3(0.0f), kDefaultSpawn.eye);
			return;
		}
		m_SplatCount = data.Count();

		// Camera spawn: prefer query/CLI params, otherwise use defaults.
		glm::vec3 eye = kDefaultSpawn.eye;
		glm::vec3 fwd = glm::normalize(kDefaultSpawn.fwd);
		if (auto s = ReadParam("eye"); s) {
			if (auto v = ParseVec3(*s); v) eye = *v;
		}
		if (auto s = ReadParam("fwd"); s) {
			if (auto v = ParseVec3(*s); v) fwd = glm::normalize(*v);
		}

		// Orbit target = alpha-weighted centroid of the loaded splats so the
		// pivot matches the actual subject regardless of which scene-specific
		// eye/fwd was passed.
		glm::vec3 target(0.0f);
		{
			glm::dvec3 sum(0.0);
			size_t kept = 0;
			for (size_t i = 0; i < data.positions.size(); ++i) {
				if (data.colors[i].a >= 32) {
					sum += glm::dvec3(data.positions[i]);
					++kept;
				}
			}
			if (kept == 0) {
				for (const auto& p : data.positions) sum += glm::dvec3(p);
				kept = std::max<size_t>(1, data.positions.size());
			}
			target = glm::vec3(sum / static_cast<double>(kept));
		}

		INFO_CORE("gsplat spawn: eye=({0},{1},{2}) target=({3},{4},{5})",
		          eye.x, eye.y, eye.z, target.x, target.y, target.z);
		SwitchCameraToOrbit(target, eye);

		m_Splats = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		m_Splats->Upload(data);

	#ifdef __EMSCRIPTEN__
		PostSceneMessage("{\"type\":\"splat-ready\"}");
	#endif
	}


	void GaussianSplatScene::OnUpdate(Timestep ts)
	{
		HandleStandardCameraArbitration(/*autoFlipOnFlyKey=*/true);
		DrainUnusedOrbitInput();

		m_Camera->Update(ts);
		const SPtr<Camera> activeRenderCam = m_Camera->GetRenderCamera();

		// Frame-interval sample (CPU wallclock between scene tick starts).
		const double frameStart = glfwGetTime();
		if (m_Splats && m_PrevFrameStart > 0.0) {
			m_Splats->Metrics().frameMs.Push(
				static_cast<float>((frameStart - m_PrevFrameStart) * 1000.0));
		}
		m_PrevFrameStart = frameStart;

		if (m_Splats) m_Splats->TickPerf();

		if (!Renderer::BeginScene(activeRenderCam)) {
			return;
		}

		// GPU sort runs before the colour pass opens.
		const double encodeStart = glfwGetTime();
		const glm::mat4& view = activeRenderCam->GetViewMatrix();
		const glm::mat4& proj = activeRenderCam->GetProjectionMatrix();
		if (m_Splats) m_Splats->EncodeSort(Renderer::Encoder(), view, proj);

		const WGPUPassTimestampWrites* renderTw =
			m_Splats ? m_Splats->GetRenderPassTimestampWrites() : nullptr;
		Renderer::OpenColorPass(0.05f, 0.05f, 0.08f, 1.0f, renderTw);
		if (m_Splats) {
			m_Splats->EncodeRender(Renderer::CurrentPass(),
			                       activeRenderCam,
			                       glm::vec2(m_ScreenWidth, m_ScreenHeight));
		}

		Renderer::ClosePass();
		if (m_Splats) m_Splats->ResolveAndReadTimestamps(Renderer::Encoder());

		const double encodeEnd = glfwGetTime();
		if (m_Splats) {
			auto& m = m_Splats->Metrics();
			m.cpuEncodeMs.Push(static_cast<float>((encodeEnd - encodeStart) * 1000.0));
			m.splatCount = static_cast<int>(m_SplatCount);
			const glm::vec3 eye = m_Camera->GetPosition();
			m.camEye[0] = eye.x;
			m.camEye[1] = eye.y;
			m.camEye[2] = eye.z;
			m.Emit();
		}

		++m_FpsCounter;
		double now = glfwGetTime();
		if (m_FpsT0 == 0.0) m_FpsT0 = now;
		if (m_FpsCounter % 60 == 0) {
			float dt = (float)(now - m_FpsT0);
			INFO_CORE("gsplat: 60 frames in {0:.3f}s = {1:.1f} fps", dt, 60.0f / dt);
			m_FpsT0 = now;

			// Periodic camera-pose dump for collecting eye/fwd for DB seeds.
			const glm::mat4& t = m_Camera->GetTransform();
			const glm::vec3 e = glm::vec3(t[3]);
			const glm::vec3 f = -glm::vec3(t[2]);
			INFO_CORE("gsplat camera: eye=({0:.3f},{1:.3f},{2:.3f}) fwd=({3:.3f},{4:.3f},{5:.3f})",
			          e.x, e.y, e.z, f.x, f.y, f.z);
		}

		// Headless screenshot hook (single-shot capture + exit).
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
