#include "GaussianSplatHqScene.h"
#include "SceneRegistry.h"

#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/RenderCommand.h"
#include "Engine/Renderer/PlyLoader.h"

#ifndef __EMSCRIPTEN__
#include <glad/glad.h>
#else
#include <GLES3/gl3.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"

using namespace Engine;

namespace Sandbox {

	namespace {

		// Target PLY under assets/splat/. truck.ply is the Inria 3DGS "Truck"
		// scene (Tanks & Temples), chosen because the paper authors publish a
		// reference render at repo-sam.inria.fr/.../ours_truck.png against
		// which we can visually judge our renderer's correctness. Override
		// via env var `HQ_PLY=splat/bonsai.ply` when cross-checking a second
		// scene to isolate asset-quality from pipeline bugs.
		constexpr const char* kDefaultPlyFile = "splat/truck.ply";

		const char* PickPlyFile()
		{
			const char* env = std::getenv("HQ_PLY");
			return (env && *env) ? env : kDefaultPlyFile;
		}


		struct Bounds {
			glm::vec3 min{0.0f};
			glm::vec3 max{0.0f};
			glm::vec3 Centre() const { return 0.5f * (min + max); }
			glm::vec3 Size()   const { return max - min; }
			float     Radius() const { return 0.5f * glm::length(Size()); }
		};

		// Percentile-trimmed axis-aligned bounds — same approach as the
		// antimatter15 .splat scene uses. Trimming 5/95 keeps sky/outlier
		// gaussians from blowing up the bbox by ~10x on wild-captured scenes.
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


	GaussianSplatHqScene::GaussianSplatHqScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat_hq", screenWidth, screenHeight)
	{
		m_Camera.SetPerspective(glm::radians(45.0f),
		                        m_ScreenWidth / m_ScreenHeight,
		                        0.1f, 10000.0f);

		namespace fs = std::filesystem;
		const fs::path plyPath = fs::path(ENGINE_ASSETS_DIR) / PickPlyFile();
		auto data = PlyLoader::LoadPly(plyPath.string());
		if (data.Empty()) {
			ERROR_CORE("GaussianSplatHqScene: failed to load {0}", plyPath.string());
			return;
		}
		m_SplatCount = data.Count();
		m_HasSH      = data.HasSH();

		// Spawn: default → hand-tuned for truck.ply matching Inria's reference
		// render (`ours_truck.png`). When HQ_PLY overrides the file we can't
		// assume the hardcoded eye/fwd make sense, so fall back to bbox auto-fit.
		const Bounds b = ComputeBounds(data);
		const bool isDefaultFile = (std::getenv("HQ_PLY") == nullptr);
		glm::vec3 eye;
		glm::vec3 fwd;
		if (isDefaultFile) {
			// Pull the camera back ~3x along the original view ray so the
			// truck occupies roughly the same frame fraction as Inria's
			// 978×550 reference render. At the hand-tuned close position
			// (-3.38, 1.29, -3.97) the truck filled ~60% of the frame; the
			// reference shows it at ~30%, so we're at 2x zoom → 2x visible
			// per-splat "mush". Matching zoom lets us actually compare.
			const glm::vec3 closeEye(-3.38f, 1.29f, -3.97f);
			fwd = glm::normalize(glm::vec3(0.53f, -0.35f, 0.77f));
			eye = closeEye - fwd * 6.0f;  // back off ~6 units along forward
		} else {
			const float radius = std::max(b.Radius(), 1.0f);
			const glm::vec3 centre = b.Centre();
			const glm::vec3 offset = glm::normalize(glm::vec3(1.0f, 0.35f, 0.8f)) * radius * 1.5f;
			eye = centre + offset;
			fwd = glm::normalize(centre - eye);
		}
		INFO_CORE("gsplat_hq bbox: min=({0},{1},{2}) max=({3},{4},{5})",
		          b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
		INFO_CORE("gsplat_hq spawn: eye=({0},{1},{2}) fwd=({3},{4},{5}) SH={6}",
		          eye.x, eye.y, eye.z, fwd.x, fwd.y, fwd.z, m_HasSH ? "on" : "off");

		m_Camera.SetTransform(glm::inverse(glm::lookAt(eye, eye + fwd, glm::vec3(0.0f, 1.0f, 0.0f))));
		m_Camera.m_MoveSpeed = std::max(b.Radius(), 1.0f) * 0.2f;

		m_Splats = std::make_unique<GaussianSplatRenderer>();
		m_Splats->Upload(data);
		m_Splats->SortNow(m_Camera.GetRenderCamera()->GetViewMatrix());
	}


	void GaussianSplatHqScene::OnUpdate(Timestep ts)
	{
		m_Camera.Update(ts);

		RenderCommand::SetClearColor({ 0.05f, 0.05f, 0.08f, 1.0f });
		RenderCommand::Clear();

		Renderer::BeginScene(m_Camera.GetRenderCamera());

		if (m_Splats) {
			m_Splats->Render(m_Camera.GetRenderCamera(),
			                 glm::vec2(m_ScreenWidth, m_ScreenHeight));
		}

		Renderer::EndScene();

		// Headless screenshot hook. If HQ_CAPTURE_PATH is set in the env,
		// grab the default framebuffer on frame 60 (first sort has fired by
		// then) and exit. Useful for iterating on render quality without
		// needing manual screenshots — run:
		//   HQ_CAPTURE_PATH=/tmp/out.png ./Sandbox --scene=gsplat_hq
		++m_FrameCount;
		if (m_FrameCount == 60) {
			const char* outPath = std::getenv("HQ_CAPTURE_PATH");
			if (outPath && *outPath) {
				const int w = static_cast<int>(m_ScreenWidth);
				const int h = static_cast<int>(m_ScreenHeight);
				std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
				glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
				// Flip vertically — OpenGL origin is bottom-left, PNG is top-left.
				std::vector<uint8_t> flipped(pixels.size());
				for (int y = 0; y < h; ++y) {
					std::memcpy(flipped.data() + (h - 1 - y) * w * 4,
					            pixels.data() + y * w * 4,
					            static_cast<size_t>(w) * 4);
				}
				stbi_write_png(outPath, w, h, 4, flipped.data(), w * 4);
				INFO_CORE("HQ capture saved to {0}", outPath);
				std::exit(0);
			}
		}
	}


	void GaussianSplatHqScene::OnImGuiRender()
	{
		ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);

		ImGui::Begin("Gaussian Splat (HQ)");
		ImGui::Text("Splats: %zu", m_SplatCount);
		ImGui::Text("SH bands 1..3: %s", m_HasSH ? "on" : "off");

		// Runtime A/B toggle between the SH-evaluated colour path and the
		// flat-colour (DC-only, .splat-equivalent) path. Disabled when the
		// asset has no SH to compare against.
		if (m_Splats && m_Splats->HasSH()) {
			bool shDisabled = m_Splats->IsSHDisabled();
			if (ImGui::Checkbox("Disable SH (flat DC only)", &shDisabled)) {
				m_Splats->SetSHDisabled(shDisabled);
			}
		}
		ImGui::Text("FPS: %.1f (%.2f ms)",
		            ImGui::GetIO().Framerate,
		            1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));

		const glm::vec3 eye = m_Camera.GetPosition();
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

	SCENE_REGISTER("gsplat_hq", GaussianSplatHqScene)

}
