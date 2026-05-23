#include "EditorScene.h"
#include "SceneRegistry.h"

#include "Engine/Application.h"
#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SplatLoader.h"
#include "Engine/VirtualInput.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace Engine;

namespace Sandbox {

	EditorScene* EditorScene::s_Current = nullptr;

	namespace {

		// Fly-camera default spawn — sits a couple of metres above origin
		// looking forward + slightly down, so the grid floor is visible.
		const glm::vec3 kSpawnPos     = glm::vec3(0.0f, 2.5f, 6.0f);
		const glm::vec3 kSpawnForward = glm::normalize(glm::vec3(0.0f, -0.25f, -1.0f));

	#ifdef __EMSCRIPTEN__
		void PostEditorMessage(const char* json) {
			EM_ASM({
				try {
					if (typeof window !== 'undefined' && window.parent !== window) {
						window.parent.postMessage(JSON.parse(UTF8ToString($0)), '*');
					}
				} catch (e) {}
			}, json);
		}
	#else
		void PostEditorMessage(const char*) {}
	#endif

	} // namespace


	EditorScene::EditorScene(float screenWidth, float screenHeight)
		: SceneBase("editor", screenWidth, screenHeight)
	{
		s_Current = this;

		const float aspect = m_ScreenWidth / m_ScreenHeight;
		m_FlyCam  .SetPerspective(glm::radians(45.0f), aspect, 0.1f, 10000.0f);
		m_OrbitCam.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 10000.0f);

		m_FlyCam.SetPose(kSpawnPos, kSpawnForward);
		// Seed an orbit pose around origin too — used when the user toggles
		// to orbit mode on an empty scene (no content to centroid on).
		m_OrbitCam.SetOrbit(glm::vec3(0.0f), kSpawnPos);

		m_Grid = std::make_unique<GridRenderer>(Application::Get().GetGfx());

		INFO_CORE("EditorScene: ready (empty, fly-cam at ({0:.2f},{1:.2f},{2:.2f}))",
		          kSpawnPos.x, kSpawnPos.y, kSpawnPos.z);

		// Tell the parent the renderer is up — the React side hides the
		// "loading renderer" overlay on this signal, same as the splat
		// catalog scenes.
		PostEditorMessage("{\"type\":\"splat-ready\"}");
		PostEditorMessage("{\"type\":\"editor-ready\"}");
	}


	EditorScene::~EditorScene()
	{
		if (s_Current == this) s_Current = nullptr;
	}


	glm::vec3 EditorScene::PickOrbitPivot(const SplatData& data) const
	{
		// Alpha-weighted centroid — drops transparent halo gaussians that
		// drag the mean off-subject. Matches GaussianSplatScene's pivot.
		glm::dvec3 sum(0.0);
		size_t kept = 0;
		for (size_t i = 0; i < data.positions.size(); ++i) {
			if (data.colors[i].a >= 32) {
				sum += glm::dvec3(data.positions[i]);
				++kept;
			}
		}
		if (kept == 0 && !data.positions.empty()) {
			for (const auto& p : data.positions) sum += glm::dvec3(p);
			kept = data.positions.size();
		}
		if (kept == 0) return glm::vec3(0.0f);
		return glm::vec3(sum / static_cast<double>(kept));
	}


	void EditorScene::LoadSplatFromBytes(const uint8_t* data, size_t size)
	{
		INFO_CORE("EditorScene: parsing {0} byte splat payload", (uint64_t)size);
		SplatData parsed = SplatLoader::LoadSplatFromBytes(data, size, "editor-upload");
		if (parsed.Empty()) {
			ERROR_CORE("EditorScene: splat parse returned no points");
			PostEditorMessage("{\"type\":\"editor-error\",\"message\":\"Failed to parse splat\"}");
			return;
		}

		const glm::vec3 pivot = PickOrbitPivot(parsed);

		if (!m_Splats) {
			m_Splats = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		}
		m_Splats->Upload(parsed);
		m_SplatCount = parsed.Count();
		m_HasContent = true;

		// Reframe orbit camera on the new subject, but keep the fly camera
		// where the user had it so a "load while flying" doesn't jump them.
		const float radius = 3.0f;
		const glm::vec3 eye = pivot + glm::vec3(0.0f, 0.4f, 1.0f) * radius;
		m_OrbitCam.SetOrbit(pivot, eye);

		INFO_CORE("EditorScene: loaded {0} splats, pivot=({1:.2f},{2:.2f},{3:.2f})",
		          (uint64_t)m_SplatCount, pivot.x, pivot.y, pivot.z);

		char buf[160];
		std::snprintf(buf, sizeof(buf),
		              "{\"type\":\"editor-splat-loaded\",\"count\":%llu}",
		              (unsigned long long)m_SplatCount);
		PostEditorMessage(buf);
	}


	void EditorScene::ClearScene()
	{
		m_Splats.reset();
		m_SplatCount = 0;
		m_HasContent = false;
		PostEditorMessage("{\"type\":\"editor-scene-cleared\"}");
	}


	void EditorScene::OnUpdate(Timestep ts)
	{
		// Camera mode arbitration — same convention as GaussianSplatScene.
		const int requested = ConsumeRequestedMode();
		if (requested == 0) {
			SetMode(CameraMode::Orbit);
		} else if (requested == 1) {
			SetMode(CameraMode::Fly);
		} else if (m_Mode == CameraMode::Orbit && AnyFlyKeyPressed()) {
			SetMode(CameraMode::Fly);
		}

		if (m_Mode == CameraMode::Orbit) {
			m_OrbitCam.Update(ts);
		} else {
			// Swallow accumulated orbit input so it doesn't snap back on
			// next mode switch.
			float discardYaw, discardPitch;
			ConsumeOrbitDeltas(discardYaw, discardPitch);
			(void)ConsumeZoomDelta();
			m_FlyCam.Update(ts);
		}

		const SPtr<Camera> activeCam = (m_Mode == CameraMode::Orbit)
		    ? m_OrbitCam.GetRenderCamera()
		    : m_FlyCam  .GetRenderCamera();

		const double frameStart = glfwGetTime();
		if (m_Splats && m_PrevFrameStart > 0.0) {
			m_Splats->Metrics().frameMs.Push(
				static_cast<float>((frameStart - m_PrevFrameStart) * 1000.0));
		}
		m_PrevFrameStart = frameStart;

		if (m_Splats) m_Splats->TickPerf();

		if (!Renderer::BeginScene(activeCam)) return;

		// Splat sort runs as a compute pass before the colour pass opens —
		// same constraint as GaussianSplatScene.
		const glm::mat4& view = activeCam->GetViewMatrix();
		const glm::mat4& proj = activeCam->GetProjectionMatrix();
		if (m_Splats) m_Splats->EncodeSort(Renderer::Encoder(), view, proj);

		const WGPUPassTimestampWrites* renderTw =
			m_Splats ? m_Splats->GetRenderPassTimestampWrites() : nullptr;

		// Lighter clear than the splat scenes — closer to a neutral
		// editor backdrop. Grid + splats blend over this.
		Renderer::OpenColorPass(0.12f, 0.13f, 0.16f, 1.0f, renderTw);

		// Grid first, then splats — splat alpha-over composites correctly
		// on whatever the grid drew.
		if (m_Grid) m_Grid->EncodeRender(Renderer::CurrentPass(), activeCam);
		if (m_Splats) {
			m_Splats->EncodeRender(Renderer::CurrentPass(),
			                       activeCam,
			                       glm::vec2(m_ScreenWidth, m_ScreenHeight));
		}

		Renderer::ClosePass();

		if (m_Splats) m_Splats->ResolveAndReadTimestamps(Renderer::Encoder());

		// Lightweight perf emit so the React overlay still works when
		// content is loaded. Skipped on empty scene (nothing to measure).
		if (m_Splats) {
			auto& m = m_Splats->Metrics();
			m.splatCount = static_cast<int>(m_SplatCount);
			const glm::vec3 eye = (m_Mode == CameraMode::Orbit)
			    ? m_OrbitCam.GetPosition()
			    : m_FlyCam  .GetPosition();
			m.camEye[0] = eye.x;
			m.camEye[1] = eye.y;
			m.camEye[2] = eye.z;
			m.Emit();
		}

		++m_FpsCounter;
		double now = glfwGetTime();
		if (m_FpsT0 == 0.0) m_FpsT0 = now;
		if (m_FpsCounter % 120 == 0) {
			float dt = (float)(now - m_FpsT0);
			INFO_CORE("editor: 120 frames in {0:.3f}s = {1:.1f} fps", dt, 120.0f / dt);
			m_FpsT0 = now;
		}

		Renderer::EndScene();
	}


	bool EditorScene::AnyFlyKeyPressed() const
	{
		return Input::IsKeyPressed(KEY_W) || Input::IsKeyPressed(KEY_A)
		    || Input::IsKeyPressed(KEY_S) || Input::IsKeyPressed(KEY_D)
		    || Input::IsKeyPressed(KEY_Q) || Input::IsKeyPressed(KEY_E);
	}


	void EditorScene::SetMode(CameraMode mode)
	{
		if (mode == m_Mode) return;

		if (mode == CameraMode::Fly) {
			const glm::vec3 eye = m_OrbitCam.GetPosition();
			m_FlyCam.SetPose(eye, m_OrbitCam.GetTarget() - eye);
		} else {
			const glm::vec3 eye    = m_FlyCam.GetPosition();
			const glm::vec3 fwd    = m_FlyCam.GetForward();
			const float     radius = m_OrbitCam.GetRadius();
			m_OrbitCam.SetOrbit(eye + fwd * radius, eye);
		}

		m_Mode = mode;

		PostEditorMessage(mode == CameraMode::Orbit
		    ? "{\"type\":\"camera-mode-changed\",\"mode\":\"orbit\"}"
		    : "{\"type\":\"camera-mode-changed\",\"mode\":\"fly\"}");
	}


	SCENE_REGISTER("editor", EditorScene)

}


// ---------------------------------------------------------------------------
// C bridge — invoked from the JS shim in Sandbox.html via Module.ccall.
// These wrappers route into the active EditorScene instance.
// ---------------------------------------------------------------------------

extern "C" {

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EDITOR_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EDITOR_EXPORT
#endif

EDITOR_EXPORT void editor_load_splat_bytes(uint8_t* data, int len)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) {
		ERROR_CORE("editor_load_splat_bytes: no live EditorScene");
		return;
	}
	if (!data || len <= 0) {
		ERROR_CORE("editor_load_splat_bytes: bad args (data={0}, len={1})",
		           (const void*)data, len);
		return;
	}
	s->LoadSplatFromBytes(data, static_cast<size_t>(len));
}

EDITOR_EXPORT void editor_clear_scene(void)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->ClearScene();
}

} // extern "C"
