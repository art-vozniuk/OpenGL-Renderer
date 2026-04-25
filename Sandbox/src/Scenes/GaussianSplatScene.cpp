#include "GaussianSplatScene.h"
#include "SceneRegistry.h"

#include "Engine/Application.h"
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SplatLoader.h"

#include <GLFW/glfw3.h>
#include <cstdlib>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

using namespace Engine;

namespace Sandbox {

	namespace {

		// Per-scene spawn location. Captured originally on native by flying
		// the camera to a pleasing angle and reading off the position +
		// forward vector. Same as the GL renderer's value.
		struct GsplatSceneSpawn {
			const char* file;
			glm::vec3   eye;
			glm::vec3   fwd;
		};

		static const GsplatSceneSpawn kTrainSpawn = {
			"splat/train.splat",
			/*eye=*/ glm::vec3(-4.60f,  0.70f,  4.30f),
			/*fwd=*/ glm::vec3( 0.49f, -0.14f, -0.86f),
		};

	} // namespace


	GaussianSplatScene::GaussianSplatScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat", screenWidth, screenHeight)
	{
		const GsplatSceneSpawn& spawn = kTrainSpawn;

		m_Camera.SetPerspective(glm::radians(45.0f),
		                        m_ScreenWidth / m_ScreenHeight,
		                        0.1f, 10000.0f);

		namespace fs = std::filesystem;
		const fs::path splatPath = fs::path(ENGINE_ASSETS_DIR) / spawn.file;
		auto data = SplatLoader::LoadSplat(splatPath.string());
		if (data.Empty()) {
			ERROR_CORE("GaussianSplatScene: failed to load {0}", splatPath.string());
			return;
		}
		m_SplatCount = data.Count();

		const glm::vec3 eye = spawn.eye;
		const glm::vec3 fwd = glm::normalize(spawn.fwd);
		INFO_CORE("gsplat spawn: eye=({0},{1},{2}) fwd=({3},{4},{5})",
		          eye.x, eye.y, eye.z, fwd.x, fwd.y, fwd.z);
		m_Camera.SetTransform(glm::inverse(glm::lookAt(eye, eye + fwd, glm::vec3(0.0f, 1.0f, 0.0f))));
		m_Camera.m_MoveSpeed = 1.0f;

		m_Splats = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		m_Splats->Upload(data);
	}


	void GaussianSplatScene::OnUpdate(Timestep ts)
	{
		m_Camera.Update(ts);

		if (!Renderer::BeginScene(m_Camera.GetRenderCamera())) {
			return;
		}

		// GPU sort runs every frame, BEFORE the colour pass is opened
		// (compute and render passes can't share an encoder once a render
		// pass is active). The renderer reads only `sortedIndices` so
		// re-sorting is just one buffer rewrite, not the bulk reshuffle
		// the GL renderer used to do.
		const glm::mat4& view = m_Camera.GetRenderCamera()->GetViewMatrix();
		if (m_Splats) m_Splats->EncodeSort(Renderer::Encoder(), view);

		Renderer::OpenColorPass(0.05f, 0.05f, 0.08f, 1.0f);
		if (m_Splats) {
			m_Splats->EncodeRender(Renderer::CurrentPass(),
			                       m_Camera.GetRenderCamera(),
			                       glm::vec2(m_ScreenWidth, m_ScreenHeight));
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


	void GaussianSplatScene::OnImGuiRender()
	{
		// ImGui is disabled on the WebGPU branch -- panel will reappear once
		// imgui_impl_wgpu is integrated.
	}

	SCENE_REGISTER("gsplat", GaussianSplatScene)

}
