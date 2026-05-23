#pragma once

#include "SceneBase.h"

#include "Engine/FlyCamera.h"
#include "Engine/OrbitCamera.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"
#include "Engine/Renderer/GridRenderer.h"

#include <memory>
#include <string>
#include <vector>

namespace Sandbox {

	/*
	 * EditorScene
	 *
	 * Empty-by-default scene for the 3D editor page. Renders a baseline
	 * grid floor and (later) any content the user pushes in via the
	 * JS bridge. Each content type lives behind its own engine renderer
	 * — GaussianSplatRenderer today, MeshRenderer next — so the editor
	 * stays orthogonal to the rendering backends.
	 *
	 * Camera: starts in fly mode so an empty scene is still navigable
	 * (orbit needs a subject). Switches to orbit on Tab once content
	 * exists, with the orbit pivot snapped to the loaded content's
	 * centroid.
	 *
	 * JS bridge (exported from the WASM module — see CMakeLists
	 * EXPORTED_FUNCTIONS):
	 *   editor_load_splat_bytes(uint8_t* ptr, size_t len)
	 *      — copy bytes into a SplatData, hot-swap into the splat renderer
	 *   editor_clear_scene()
	 *      — drop any loaded content and return the scene to the empty state
	 */
	class EditorScene final : public SceneBase
	{
	public:
		EditorScene(float screenWidth, float screenHeight);
		~EditorScene() override;

		void OnUpdate(Engine::Timestep ts) override;

		// Called from the C bridge (extern "C" wrappers in the .cpp).
		// Both functions are safe to call before OnUpdate runs at least
		// once — they only touch the splat renderer via the engine's
		// queue, no per-frame state.
		void LoadSplatFromBytes(const uint8_t* data, size_t size);
		void ClearScene();

		// Singleton-style instance pointer so the C entry points can find
		// the live scene. Set in the constructor, cleared in the dtor.
		// We only ever have one active scene, so this is fine.
		static EditorScene* Current() { return s_Current; }

	private:
		enum class CameraMode { Orbit = 0, Fly = 1 };

		void SetMode(CameraMode mode);
		bool AnyFlyKeyPressed() const;

		// Picks a sensible orbit pivot from the splat point cloud (alpha-
		// weighted centroid, same robustness trick as GaussianSplatScene).
		glm::vec3 PickOrbitPivot(const Engine::SplatData& data) const;

		Engine::OrbitCamera m_OrbitCam;
		Engine::FlyCamera   m_FlyCam;
		CameraMode          m_Mode = CameraMode::Fly;
		bool                m_HasContent = false;

		std::unique_ptr<Engine::GridRenderer>          m_Grid;
		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;
		size_t                                         m_SplatCount = 0;

		double m_PrevFrameStart = 0.0;
		int    m_FpsCounter     = 0;
		double m_FpsT0          = 0.0;

		static EditorScene* s_Current;
	};

}
