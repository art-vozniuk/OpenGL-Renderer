#pragma once

#include "SceneBase.h"

#include "Engine/FlyCamera.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"

#include <memory>

namespace Sandbox {

	/*
	 * GaussianSplatHqScene
	 * --------------------
	 * Full-quality Gaussian-splat scene loaded from an Inria .ply file. Same
	 * render path as GaussianSplatScene but the data source keeps SH bands
	 * 1..3 so view-dependent highlights survive. Used as the "max quality"
	 * reference for comparing against the compact antimatter15 .splat path.
	 */
	class GaussianSplatHqScene final : public SceneBase
	{
	public:
		GaussianSplatHqScene(float screenWidth, float screenHeight);

		void OnUpdate(Engine::Timestep ts) override;
		void OnImGuiRender() override;

	private:
		Engine::FlyCamera m_Camera;
		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;
		size_t m_SplatCount = 0;
		bool   m_HasSH      = false;
		int    m_FrameCount = 0;
	};

}
