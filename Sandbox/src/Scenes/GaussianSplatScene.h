#pragma once

#include "SceneBase.h"

#include "Engine/FlyCamera.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"

#include <memory>

namespace Sandbox {

	/*
	 * GaussianSplatScene
	 *
	 * Single-asset scene: loads an antimatter15 .splat file and drives
	 * the GS renderer (per-frame GPU sort + indirected draw). Camera is
	 * a simple WASD fly camera.
	 */
	class GaussianSplatScene final : public SceneBase
	{
	public:
		GaussianSplatScene(float screenWidth, float screenHeight);

		void OnUpdate(Engine::Timestep ts) override;

	private:
		Engine::FlyCamera m_Camera;
		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;
		size_t m_SplatCount = 0;
		int    m_FrameCount = 0;
		int    m_FpsCounter = 0;
		double m_FpsT0      = 0.0;
	};

}
