#pragma once

#include "SceneBase.h"
#include "Engine/FlyCamera.h"

namespace Sandbox {

	/*
	 * GaussianSplatScene (placeholder)
	 * --------------------------------
	 * Stub scene registered under the "gsplat" id. Currently just clears to
	 * a distinctive color and wires the fly-camera so we can verify that the
	 * scene-selection plumbing reaches the renderer. Will be fleshed out in
	 * a follow-up pass that adds actual splat loading + rendering.
	 */
	class GaussianSplatScene final : public SceneBase
	{
	public:
		GaussianSplatScene(float screenWidth, float screenHeight);

		void OnUpdate(Engine::Timestep ts) override;
		void OnImGuiRender() override;

	private:
		Engine::FlyCamera m_Camera;
	};

}
