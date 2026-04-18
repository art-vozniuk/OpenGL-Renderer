#include "GaussianSplatScene.h"
#include "SceneRegistry.h"

// Include only the engine headers this scene uses. <Engine.h> would pull
// in EntryPoint.h (defines main()) and produce duplicate-symbol errors
// when multiple scene TUs include it.
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/RenderCommand.h"
#include "Engine/Renderer/SplatLoader.h"

#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"

using namespace Engine;

namespace Sandbox {

	namespace {

		// Simple axis-aligned bounds — used to place the fly camera somewhere
		// from which the splat cloud is actually visible at app start.
		struct Bounds {
			glm::vec3 min{std::numeric_limits<float>::max()};
			glm::vec3 max{std::numeric_limits<float>::lowest()};

			glm::vec3 Centre() const { return 0.5f * (min + max); }
			glm::vec3 Size()   const { return max - min; }
			float     Radius() const { return 0.5f * glm::length(Size()); }
		};

		Bounds ComputeBounds(const SplatData& d)
		{
			Bounds b;
			for (const auto& p : d.positions) {
				b.min = glm::min(b.min, p);
				b.max = glm::max(b.max, p);
			}
			return b;
		}

	}


	GaussianSplatScene::GaussianSplatScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat", screenWidth, screenHeight)
	{
		m_Camera.SetPerspective(glm::radians(45.0f), m_ScreenWidth / m_ScreenHeight, 0.1f, 10000.0f);

		// Load splats from assets/splat/ — packaged into Sandbox.data on web,
		// resolved via ENGINE_ASSETS_DIR on native.
		namespace fs = std::filesystem;
		const fs::path splatPath = fs::path(ENGINE_ASSETS_DIR) / "splat" / "train.splat";
		auto data = SplatLoader::LoadSplat(splatPath.string());
		if (data.Empty()) {
			ERROR_CORE("GaussianSplatScene: failed to load {0}", splatPath.string());
			return;
		}
		m_SplatCount = data.Count();

		// Place the camera so the whole cloud fits in view. Position is
		// roughly `centre + (0, 0, radius * 2)` looking toward the centre.
		const Bounds b = ComputeBounds(data);
		const glm::vec3 centre = b.Centre();
		const float radius = std::max(b.Radius(), 1.0f);
		const glm::vec3 eye = centre + glm::vec3(0.0f, 0.0f, radius * 2.0f);
		INFO_CORE("gsplat bounds: centre=({0},{1},{2}) radius={3}",
		          centre.x, centre.y, centre.z, radius);
		m_Camera.SetTransform(glm::inverse(glm::lookAt(eye, centre, glm::vec3(0.0f, 1.0f, 0.0f))));
		m_Camera.m_MoveSpeed = radius * 0.5f;  // scale WASD speed with scene

		m_Splats = std::make_unique<GaussianSplatRenderer>();
		m_Splats->Upload(data);
	}


	void GaussianSplatScene::OnUpdate(Timestep ts)
	{
		m_Camera.Update(ts);

		// Deep grey clear colour so visible splats stand out without looking
		// like they sit on pitch black (which would hide low-opacity edges).
		RenderCommand::SetClearColor({ 0.05f, 0.05f, 0.08f, 1.0f });
		RenderCommand::Clear();

		Renderer::BeginScene(m_Camera.GetRenderCamera());

		if (m_Splats) {
			m_Splats->Render(m_Camera.GetRenderCamera(),
			                 glm::vec2(m_ScreenWidth, m_ScreenHeight));
		}

		Renderer::EndScene();
	}


	void GaussianSplatScene::OnImGuiRender()
	{
		ImGui::Begin("Gaussian Splat");
		ImGui::Text("Splats: %zu", m_SplatCount);
		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
		ImGui::End();
	}

	SCENE_REGISTER("gsplat", GaussianSplatScene)

}
