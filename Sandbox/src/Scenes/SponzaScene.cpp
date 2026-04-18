#include "SponzaScene.h"
#include "SceneRegistry.h"

// Include only the engine headers this scene uses. <Engine.h> would pull
// in EntryPoint.h (defines main()) and produce duplicate-symbol errors
// when multiple scene TUs include it.
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/RenderCommand.h"
#include "Engine/Renderer/Texture.h"
#include "Engine/Core/Math.h"
#include "Engine/Renderer/Assets.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"

using namespace Engine;

namespace Sandbox {

	SponzaScene::SponzaScene(float screenWidth, float screenHeight)
		: SceneBase("sponza", screenWidth, screenHeight)
		, m_Skybox(std::make_shared<Scn::SkyBox>())
		, m_ScreenQuad(std::make_shared<Scn::Quad>(glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 0.f))))
	{
		m_Model = AssetManager::GetModel("sponza/sponza.gltf");
		m_Camera.SetPerspective(glm::radians(45.0f), m_ScreenWidth / m_ScreenHeight, 0.1f, 10000.0f);

		auto cubeMap = AssetManager::GetCubemap("cube2");
		m_Skybox->AddTexture(cubeMap, Scn::Texture::Type::Cubemap);
		m_Model->BindCubemap(cubeMap);

		// Warm up caches for shaders we need each frame.
		AssetManager::GetShader(m_ScreenShader);
		AssetManager::GetShader(m_DefaultShader);

		auto lightShader = AssetManager::GetShader(m_LightSourceShader);
		lightShader->Bind();
		lightShader->UploadUniformFloat3("u_Color", glm::vec3(1.f, 1.f, 1.f));

		// One animated point light + one (disabled) spot light in the array so
		// the shader sampler count matches the uniform array declaration.
		m_ScnLight.pointLights.emplace_back();
		m_ScnLight.spotLights.emplace_back();

		auto& dl = m_ScnLight.dirLight;
		dl.direction = glm::vec3(-0.2f, -1.0f, -0.3f);
		dl.ambient   = glm::vec3(0.1f, 0.1f, 0.1f);
		dl.diffuse   = glm::vec3(0.4f, 0.4f, 0.4f);
		dl.specular  = glm::vec3(0.5f, 0.5f, 0.5f);

		auto& pl = m_ScnLight.pointLights[0];
		pl.position  = glm::vec3(0.f, 1.5f, 0.f);
		pl.ambient   = glm::vec3(0.05f, 0.05f, 0.05f);
		pl.diffuse   = glm::vec3(0.8f, 0.8f, 0.8f);
		pl.specular  = glm::vec3(1.0f, 1.0f, 1.0f);
		pl.constant  = 1.0f;
		pl.linear    = 0.09f;
		pl.quadratic = 0.32f;

		auto& sl = m_ScnLight.spotLights[0];
		sl.position = m_Camera.GetPosition();
		Math::matGetForward(m_Camera.GetTransform(), sl.direction);
		sl.ambient     = glm::vec3(0.0f);
		sl.diffuse     = glm::vec3(1.0f);
		sl.specular    = glm::vec3(1.0f);
		sl.constant    = 1.0f;
		sl.linear      = 0.0009f;
		sl.quadratic   = 0.00032f;
		sl.cutOff      = glm::cos(glm::radians(12.5f));
		sl.outerCutOff = glm::cos(glm::radians(15.0f));

		for (const auto& l : m_ScnLight.pointLights)
			m_LightSources.emplace_back(std::make_shared<Scn::Cube>(glm::translate(glm::mat4(1.f), l.position), 0.05f));

		// Offscreen framebuffer for the optional post-processing pass.
		m_ScreenFrameBuffer.reset(FrameBuffer::Create());
		m_ScreenFrameBuffer->Bind();

		auto tex2d = Texture2D::Create(nullptr, (int)m_ScreenWidth, (int)m_ScreenHeight, 3);
		auto texture = m_ScreenQuad->AddTexture(tex2d, Scn::Texture::Type::Diffuse);
		m_ScreenFrameBuffer->AddTexture(texture->GetRenderTex());

		SPtr<RenderBuffer> renderBuffer;
		renderBuffer.reset(RenderBuffer::Create((int)m_ScreenWidth, (int)m_ScreenHeight));
		m_ScreenFrameBuffer->AddRenderBuffer(renderBuffer);

		m_PostProcessReady = m_ScreenFrameBuffer->Check();
		if (!m_PostProcessReady) {
			ERROR_CORE("Post-processing framebuffer failed, falling back to direct rendering");
		}
		m_ScreenFrameBuffer->Unbind();
	}


	void SponzaScene::OnUpdate(Timestep ts)
	{
		m_Camera.Update(ts);

		RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.f });
		RenderCommand::Clear();

		m_LightSources[0]->SetTransform(glm::translate(glm::mat4(1.f), m_ScnLight.pointLights[0].position));

		Renderer::BeginScene(m_Camera.GetRenderCamera());

		auto& sl = m_ScnLight.spotLights[0];
		sl.position = m_Camera.GetPosition();
		Math::matGetForward(m_Camera.GetTransform(), sl.direction);

		if (m_EnablePostProcessing && m_PostProcessReady)
		{
			m_ScreenFrameBuffer->Bind();
			RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1.f });
			RenderCommand::Clear();

			RenderScene();

			m_ScreenFrameBuffer->Unbind();
			RenderCommand::SetClearColor({ 1.f, 1.f, 1.f, 1.f });
			RenderCommand::Clear();

			auto screenShader = AssetManager::GetShader(m_ScreenShader);
			screenShader->Bind();
			screenShader->UploadUniformFloat("u_dbgPPOffset", m_DbgPPOffset);
			screenShader->UploadUniformInt("u_dbgPPEffect", m_DbgPPEffect);
			m_ScreenQuad->Render(screenShader);
		}
		else
		{
			RenderScene();
		}

		Renderer::EndScene();

		AnimatePointLight(ts);
	}


	void SponzaScene::OnImGuiRender()
	{
		auto& dl = m_ScnLight.dirLight;
		auto& pl = m_ScnLight.pointLights[0];
		auto& sl = m_ScnLight.spotLights[0];

		ImGui::Begin("Settings");

		ImGui::SliderFloat3("Dir light direction", glm::value_ptr(dl.direction), -1.f, 0.f);
		ImGui::ColorEdit3("Dir light ambient",  glm::value_ptr(dl.ambient));
		ImGui::ColorEdit3("Dir light diffuse",  glm::value_ptr(dl.diffuse));
		ImGui::ColorEdit3("Dir light specular", glm::value_ptr(dl.specular));

		ImGui::SliderFloat3("Point light position",  glm::value_ptr(pl.position), -12.f, 12.f);
		ImGui::ColorEdit3("Point light ambient",     glm::value_ptr(pl.ambient));
		ImGui::ColorEdit3("Point light diffuse",     glm::value_ptr(pl.diffuse));
		ImGui::ColorEdit3("Point light specular",    glm::value_ptr(pl.specular));
		ImGui::SliderFloat("Point light constant",   &pl.constant, 0.f, 1.f);
		ImGui::SliderFloat("Point light linear",     &pl.linear,   0.f, 0.001f);
		ImGui::SliderFloat("Point light quadratic",  &pl.quadratic, 0.f, 0.001f);

		ImGui::ColorEdit3("Spot light ambient",  glm::value_ptr(sl.ambient));
		ImGui::ColorEdit3("Spot light diffuse",  glm::value_ptr(sl.diffuse));
		ImGui::ColorEdit3("Spot light specular", glm::value_ptr(sl.specular));
		ImGui::SliderFloat("Spot light constant",  &sl.constant, 0.f, 1.f);
		ImGui::SliderFloat("Spot light linear",    &sl.linear,   0.f, 0.001f);
		ImGui::SliderFloat("Spot light quadratic", &sl.quadratic, 0.f, 0.001f);

		ImGui::Checkbox("Disable normal mapping", &m_DbgDisableNormalMapping);
		ImGui::Checkbox("Enable post processing", &m_EnablePostProcessing);

		ImGui::SliderFloat("Post proc offset", &m_DbgPPOffset, 0.f, 0.01f);
		ImGui::SliderInt  ("Post proc effect", &m_DbgPPEffect, 0, 3);

		ImGui::End();
	}


	void SponzaScene::RenderScene()
	{
		auto defaultShader = AssetManager::GetShader(m_DefaultShader);
		defaultShader->Bind();
		defaultShader->UploadUniformsDefaultLighting(m_ScnLight, m_Camera.GetPosition());
		defaultShader->UploadUniformInt("u_dbgDisableNormalMapping", m_DbgDisableNormalMapping ? 1 : 0);

		m_Model->SetTransform(glm::mat4(1.f));
		m_Model->Render(defaultShader);

		// Skybox: draw with cull disabled so the inward-facing faces are kept.
		RenderCommand::CullFaces(false);
		auto skyboxShader = AssetManager::GetShader(m_SkyboxShader);
		m_Skybox->Render(skyboxShader);
		RenderCommand::CullFaces(true);

		auto lightShader = AssetManager::GetShader(m_LightSourceShader);
		for (const auto& ls : m_LightSources)
			ls->Render(lightShader);
	}


	void SponzaScene::AnimatePointLight(Timestep ts)
	{
		constexpr float max_x = 8.f;
		constexpr float max_z = 1.2f;
		constexpr float y     = 1.5f;
		constexpr float speed_x = 0.8f;
		constexpr float speed_z = 0.4f;

		static int direction_x = 1;
		static int direction_z = 1;
		auto& pos = m_ScnLight.pointLights[0].position;
		pos.y = y;

		pos.x += direction_x * speed_x * ts;
		if (std::abs(pos.x) > max_x) {
			pos.x = max_x * direction_x;
			direction_x *= -1;
		}

		pos.z += direction_z * speed_z * ts;
		if (std::abs(pos.z) > max_z) {
			pos.z = max_z * direction_z;
			direction_z *= -1;
		}
	}

	SCENE_REGISTER("sponza", SponzaScene)

}
