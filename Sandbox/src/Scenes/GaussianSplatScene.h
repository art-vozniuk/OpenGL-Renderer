#pragma once

#include "SceneBase.h"

#include "Engine/Renderer/GaussianSplatRenderer.h"

#include <memory>

namespace Sandbox {

	/*
	 * GaussianSplatScene
	 *
	 * Single-asset scene: loads an antimatter15 .splat file and drives
	 * the GS renderer (per-frame GPU sort + indirected draw).
	 *
	 * Two camera modes (desktop only — mobile sticks to orbit). The
	 * mode swap, key arbitration, and pose preservation all live in
	 * SceneBase; this class just owns the splat data and renders it.
	 */
	class GaussianSplatScene final : public SceneBase
	{
	public:
		GaussianSplatScene(float screenWidth, float screenHeight);

		void OnUpdate(Engine::Timestep ts) override;

	private:
		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;
		size_t m_SplatCount     = 0;
		int    m_FrameCount     = 0;
		int    m_FpsCounter     = 0;
		double m_FpsT0          = 0.0;
		double m_PrevFrameStart = 0.0;
	};

}
