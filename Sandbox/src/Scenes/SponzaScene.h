#pragma once

#include <vector>

#include "SceneBase.h"

#include "Engine/FlyCamera.h"
#include "Engine/Lighting.h"
#include "Engine/Scene.h"
#include "Engine/Renderer/Buffer.h"

namespace Sandbox {

	/*
	 * SponzaScene
	 * -----------
	 * Classic Phong-lit forward-rendered Sponza — the Sandbox's default
	 * scene. All resources (model, shaders, cubemap, lights, post-process
	 * framebuffer) are owned here; the Application drives it via the
	 * Layer OnUpdate / OnImGuiRender hooks.
	 */
	class SponzaScene final : public SceneBase
	{
	public:
		SponzaScene(float screenWidth, float screenHeight);

		void OnUpdate(Engine::Timestep ts) override;
		void OnImGuiRender() override;
		void OnEvent(Engine::Event&) override {}

	private:
		void RenderScene();
		void AnimatePointLight(Engine::Timestep ts);

	private:
		Engine::SceneLight m_ScnLight;
		Engine::SPtr<Engine::Scn::Model> m_Model;

		std::vector<Engine::SPtr<Engine::Scn::Cube>> m_LightSources;

		Engine::SPtr<Engine::Scn::SkyBox> m_Skybox;
		Engine::SPtr<Engine::Scn::Quad>   m_ScreenQuad;

		// Shader names resolved through AssetManager on demand.
		std::string m_DefaultShader     = "default";
		std::string m_LightSourceShader = "flat_color";
		std::string m_ScreenShader      = "default_screen";
		std::string m_SkyboxShader      = "default_skybox";

		Engine::SPtr<Engine::FrameBuffer> m_ScreenFrameBuffer;
		bool m_PostProcessReady = false;
		bool m_EnablePostProcessing = false;

		bool  m_DbgDisableNormalMapping = false;
		float m_DbgPPOffset = 0.003f;
		int   m_DbgPPEffect = 0;

		Engine::FlyCamera m_Camera;
	};

}
