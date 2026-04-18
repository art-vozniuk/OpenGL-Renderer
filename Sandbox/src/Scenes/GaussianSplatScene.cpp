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

		// Percentile-trimmed axis-aligned bounds — used to place the fly
		// camera somewhere from which the splat cloud is actually visible
		// at app start. Trained splat scenes contain a handful of outlier
		// Gaussians at sky/background ranges that would otherwise inflate
		// min/max bounds by 5-10x and put the camera too far away.
		struct Bounds {
			glm::vec3 min{0.0f};
			glm::vec3 max{0.0f};

			glm::vec3 Centre() const { return 0.5f * (min + max); }
			glm::vec3 Size()   const { return max - min; }
			float     Radius() const { return 0.5f * glm::length(Size()); }
		};

		// Returns the P-th / (1-P)-th percentile on each axis independently.
		// Using 5% / 95% kills the top-and-bottom 5% of outliers per axis
		// which is enough to recover a sane bbox for typical reconstructed
		// Gaussian scenes.
		Bounds ComputeBounds(const SplatData& d, float trim = 0.05f)
		{
			Bounds b;
			if (d.Empty()) return b;

			std::vector<float> buf(d.positions.size());
			for (int axis = 0; axis < 3; ++axis) {
				for (size_t i = 0; i < d.positions.size(); ++i) buf[i] = d.positions[i][axis];
				size_t lo = static_cast<size_t>(buf.size() * trim);
				size_t hi = buf.size() - 1 - lo;
				std::nth_element(buf.begin(), buf.begin() + lo, buf.end());
				float lowVal = buf[lo];
				std::nth_element(buf.begin() + lo + 1, buf.begin() + hi, buf.end());
				float highVal = buf[hi];
				b.min[axis] = lowVal;
				b.max[axis] = highVal;
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

		// Place the camera so the whole cloud fits comfortably in view.
		// Using ~3 × radius gives enough margin that the splats read as a
		// distinct object rather than filling the viewport from inside.
		const Bounds b = ComputeBounds(data);
		const glm::vec3 centre = b.Centre();
		const float radius = std::max(b.Radius(), 1.0f);
		const glm::vec3 eye = centre + glm::vec3(0.0f, 0.0f, radius * 3.0f);
		INFO_CORE("gsplat bbox: min=({0},{1},{2}) max=({3},{4},{5})",
		          b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
		INFO_CORE("gsplat camera: eye=({0},{1},{2}) lookAt=({3},{4},{5}) r={6}",
		          eye.x, eye.y, eye.z, centre.x, centre.y, centre.z, radius);
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
		ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(260.0f, 0.0f), ImGuiCond_FirstUseEver);

		ImGui::Begin("Gaussian Splat");
		ImGui::Text("Splats: %zu", m_SplatCount);
		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
		ImGui::Separator();
		const glm::vec3 eye = m_Camera.GetPosition();
		ImGui::Text("Eye: (%.1f, %.1f, %.1f)", eye.x, eye.y, eye.z);
		ImGui::End();
	}

	SCENE_REGISTER("gsplat", GaussianSplatScene)

}
