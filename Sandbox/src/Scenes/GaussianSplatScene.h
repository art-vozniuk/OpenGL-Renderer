#pragma once

#include "SceneBase.h"

#include "Engine/FlyCamera.h"
#include "Engine/OrbitCamera.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"

#include <memory>

namespace Sandbox {

	/*
	 * GaussianSplatScene
	 *
	 * Single-asset scene: loads an antimatter15 .splat file and drives
	 * the GS renderer (per-frame GPU sort + indirected draw).
	 *
	 * Two camera modes (desktop only — mobile sticks to orbit):
	 *   - Orbit (default): touch / mouse drag orbits the splat centroid,
	 *     scroll / pinch zooms; soft-elastic limits snap back to the
	 *     auto-framed pose.
	 *   - Fly: free-flight with WASDEQ + mouse-look. Entered by pressing
	 *     any of WASDEQ in orbit mode, or via the parent-frame toolbar
	 *     (postMessage → vinput_request_mode). Switching back to orbit
	 *     drops the fly pose and snaps to the auto-framed view.
	 */
	class GaussianSplatScene final : public SceneBase
	{
	public:
		GaussianSplatScene(float screenWidth, float screenHeight);

		void OnUpdate(Engine::Timestep ts) override;

	private:
		enum class CameraMode { Orbit = 0, Fly = 1 };

		// Switches the active camera, copying pose orbit → fly on entry
		// and posting 'camera-mode-changed' to the parent frame.
		void SetMode(CameraMode mode);
		bool AnyFlyKeyPressed() const;

		Engine::OrbitCamera m_OrbitCam;
		Engine::FlyCamera   m_FlyCam;
		CameraMode          m_Mode = CameraMode::Orbit;

		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;
		size_t m_SplatCount   = 0;
		int    m_FrameCount   = 0;
		int    m_FpsCounter   = 0;
		double m_FpsT0        = 0.0;
		double m_PrevFrameStart = 0.0;  // for frame-interval ms metric
	};

}
