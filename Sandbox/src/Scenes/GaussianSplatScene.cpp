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


	namespace {

		// Per-scene spawn. Captured by flying the fly-cam to a pleasing
		// angle and reading the Eye / Fwd lines from the ImGui panel.
		struct GsplatSceneSpawn {
			const char* file;
			glm::vec3   eye;   // world-space camera position
			glm::vec3   fwd;   // world-space forward direction (unit)
		};

		// When we add more scenes we'll look this up by id on the
		// SceneBase; for now train.splat is the only option.
		static const GsplatSceneSpawn kTrainSpawn = {
			"splat/train.splat",
			/*eye=*/ glm::vec3(-4.60f,  0.70f,  4.30f),
			/*fwd=*/ glm::vec3( 0.49f, -0.14f, -0.86f),
		};

	}


	GaussianSplatScene::GaussianSplatScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat", screenWidth, screenHeight)
	{
		const GsplatSceneSpawn& spawn = kTrainSpawn;

		m_Camera.SetPerspective(glm::radians(45.0f), m_ScreenWidth / m_ScreenHeight, 0.1f, 10000.0f);

		// Load splats from assets/splat/ — packaged into Sandbox.data on web,
		// resolved via ENGINE_ASSETS_DIR on native.
		namespace fs = std::filesystem;
		const fs::path splatPath = fs::path(ENGINE_ASSETS_DIR) / spawn.file;
		auto data = SplatLoader::LoadSplat(splatPath.string());
		if (data.Empty()) {
			ERROR_CORE("GaussianSplatScene: failed to load {0}", splatPath.string());
			return;
		}
		m_SplatCount = data.Count();

		const Bounds b = ComputeBounds(data);
		const float radius = std::max(b.Radius(), 1.0f);
		const glm::vec3 eye = spawn.eye;
		const glm::vec3 fwd = glm::normalize(spawn.fwd);
		INFO_CORE("gsplat bbox: min=({0},{1},{2}) max=({3},{4},{5})",
		          b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
		INFO_CORE("gsplat spawn: eye=({0},{1},{2}) fwd=({3},{4},{5})",
		          eye.x, eye.y, eye.z, fwd.x, fwd.y, fwd.z);
		// Splats are already converted to +Y-up by SplatLoader. The look-at
		// target is just `eye + fwd` — lookAt only cares about direction.
		m_Camera.SetTransform(glm::inverse(glm::lookAt(eye, eye + fwd, glm::vec3(0.0f, 1.0f, 0.0f))));
		m_Camera.m_MoveSpeed = radius * 0.2f;

		m_Splats = std::make_unique<GaussianSplatRenderer>();
		m_Splats->Upload(data);
		// Sort once up-front so the very first rendered frame is already
		// back-to-front — otherwise the user catches a one-frame flash of
		// file-order blending before the in-Render sort fires.
		m_Splats->SortNow(m_Camera.GetRenderCamera()->GetViewMatrix());
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
		ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);

		ImGui::Begin("Gaussian Splat");
		ImGui::Text("Splats: %zu", m_SplatCount);
		ImGui::Text("FPS: %.1f (%.2f ms)",
		            ImGui::GetIO().Framerate,
		            1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));

		const glm::vec3 eye = m_Camera.GetPosition();
		// Forward direction = camera-local -Z mapped to world. The camera's
		// world transform has -Z as its third column negated; forward is
		// therefore -transform[2].xyz.
		const glm::mat4& cam = m_Camera.GetTransform();
		const glm::vec3 fwd = -glm::vec3(cam[2]);
		ImGui::Text("Eye: (%.2f, %.2f, %.2f)", eye.x, eye.y, eye.z);
		ImGui::Text("Fwd: (%.2f, %.2f, %.2f)", fwd.x, fwd.y, fwd.z);

		if (m_Splats) {
			const auto last = m_Splats->LastFrame();
			const auto peak = m_Splats->MaxLast5s();
			ImGui::Separator();
			ImGui::TextUnformatted("Per-stage (ms)  this / peak 5s");
			ImGui::Text("  sort       %6.2f / %6.2f", last.sortMs,      peak.sortMs);
			ImGui::Text("  reshuffle  %6.2f / %6.2f", last.reshuffleMs, peak.reshuffleMs);
			ImGui::Text("  upload     %6.2f / %6.2f", last.uploadMs,    peak.uploadMs);
			ImGui::Text("  draw       %6.2f / %6.2f", last.drawMs,      peak.drawMs);
		}
		ImGui::End();
	}

	SCENE_REGISTER("gsplat", GaussianSplatScene)

}
