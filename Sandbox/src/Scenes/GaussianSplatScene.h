#pragma once

#include "SceneBase.h"

#include "Engine/FlyCamera.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"

#include <memory>

namespace Sandbox {

	/*
	 * GaussianSplatScene
	 * ------------------
	 * Renders a single static Gaussian-splat scene loaded from an
	 * antimatter15-format .splat file. Phase 1 milestone — no streaming,
	 * no CPU sort yet (alpha blend artifacts are expected on this pass).
	 */
	class GaussianSplatScene final : public SceneBase
	{
	public:
		GaussianSplatScene(float screenWidth, float screenHeight);

		void OnUpdate(Engine::Timestep ts) override;
		void OnImGuiRender() override;

	private:
		Engine::FlyCamera m_Camera;
		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;
		size_t m_SplatCount = 0;
	};

}
