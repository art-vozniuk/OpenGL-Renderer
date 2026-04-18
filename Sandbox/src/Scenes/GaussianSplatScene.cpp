#include "GaussianSplatScene.h"
#include "SceneRegistry.h"

// Include only the engine headers this scene uses. <Engine.h> would pull
// in EntryPoint.h (defines main()) and produce duplicate-symbol errors
// when multiple scene TUs include it.
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/RenderCommand.h"

#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"

using namespace Engine;

namespace Sandbox {

	GaussianSplatScene::GaussianSplatScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat", screenWidth, screenHeight)
	{
		m_Camera.SetPerspective(glm::radians(45.0f), m_ScreenWidth / m_ScreenHeight, 0.1f, 10000.0f);
	}


	void GaussianSplatScene::OnUpdate(Timestep ts)
	{
		m_Camera.Update(ts);

		// Distinctive clear color so we can tell at a glance that the stub
		// scene is active (vs. the Sponza grey).
		RenderCommand::SetClearColor({ 0.08f, 0.02f, 0.16f, 1.f });
		RenderCommand::Clear();

		Renderer::BeginScene(m_Camera.GetRenderCamera());
		// TODO: splat rendering goes here.
		Renderer::EndScene();
	}


	void GaussianSplatScene::OnImGuiRender()
	{
		ImGui::Begin("Gaussian Splat (stub)");
		ImGui::TextUnformatted("Placeholder scene.");
		ImGui::TextUnformatted("Splat loader / renderer arrives in a follow-up pass.");
		ImGui::End();
	}

	SCENE_REGISTER("gsplat", GaussianSplatScene)

}
