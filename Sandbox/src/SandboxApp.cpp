#include <Engine.h>

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <GLFW/glfw3.h>

#include <cmath>
#include "Engine/FlyCamera.h"

#ifndef __EMSCRIPTEN__
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#endif

#include "Engine/Scene.h"
#include "Engine/Lighting.h"
#include "Engine/Core/Math.h"
#include "Engine/Renderer/Assets.h"

//#include "Glad/include/glad/glad.h"

using namespace Engine;

class ExampleLayer : public Layer
{
public:
	ExampleLayer(float screenWidth, float screenHeight)
		: Layer("Example")
	{
#ifndef __EMSCRIPTEN__
		m_Model = AssetManager::GetModel("sponza/sponza.obj");
#else
		m_Model = AssetManager::GetModel("sponza/sponza.gltf");
#endif

		m_Camera.SetPerspective(glm::radians(45.0f), screenWidth / screenHeight, 0.1f, 10000.0f);

		auto cubeMap = AssetManager::GetCubemap("cube2");
		m_Skybox->AddTexture(cubeMap, Scn::Texture::Type::Cubemap);
		m_Model->BindCubemap(cubeMap);

		AssetManager::GetShader(m_ScreenShader);
		AssetManager::GetShader(m_DefaultShader);

		auto lightShader = AssetManager::GetShader(m_LightSourceShader);
		lightShader->Bind();
		lightShader->UploadUniformFloat3("u_Color", glm::vec3(1.f, 1.f, 1.f));

		m_ScnLight.pointLights.emplace_back();
		m_ScnLight.spotLights.emplace_back();

		auto& dl = m_ScnLight.dirLight;
		dl.direction = glm::vec3(-0.2f, -1.0f, -0.3f);
		dl.ambient = glm::vec3(0.1f, 0.1f, 0.1f);
		dl.diffuse = glm::vec3(0.4f, 0.4f, 0.4f);
		dl.specular = glm::vec3(0.5f, 0.5f, 0.5f);

		auto& pl = m_ScnLight.pointLights[0];
		pl.position = glm::vec3(6.f, 14.f, 0.f);
		pl.ambient = glm::vec3(1.f, 1.f, 1.f);
		pl.diffuse = glm::vec3(0.8f, 0.8f, 0.8f);
		pl.specular = glm::vec3(1.0f, 1.0f, 1.0f);
		pl.constant = 1.0f;
		pl.linear = 0.00009f;
		pl.quadratic = 0.000032f;

		auto& sl = m_ScnLight.spotLights[0];
		sl.position = m_Camera.GetPosition();
		Math::matGetForward(m_Camera.GetTransform(), sl.direction);
		sl.ambient = glm::vec3(0.0f, 0.0f, 0.0f);
		sl.diffuse = glm::vec3(1.0f, 1.0f, 1.0f);
		sl.specular = glm::vec3(1.0f, 1.0f, 1.0f);
		sl.constant = 1.0f;
		sl.linear = 0.0009f;
		sl.quadratic = 0.00032f;
		sl.cutOff = glm::cos(glm::radians(12.5f));
		sl.outerCutOff = glm::cos(glm::radians(15.0f));

		for (const auto& l : m_ScnLight.pointLights)
			m_LightSources.emplace_back(std::make_shared<Scn::Cube>(glm::translate(glm::mat4(1.f), l.position), 1.f));

		m_ScreenFrameBuffer.reset(FrameBuffer::Create());
		m_ScreenFrameBuffer->Bind();

		auto tex2d = Texture2D::Create(nullptr, (int)screenWidth, (int)screenHeight, 3);
		auto texture = m_ScreenQuad->AddTexture(tex2d, Scn::Texture::Type::Diffuse);

		m_ScreenFrameBuffer->AddTexture(texture->GetRenderTex());

		SPtr<RenderBuffer> renderBuffer;
		renderBuffer.reset(RenderBuffer::Create((int)screenWidth, (int)screenHeight));
		m_ScreenFrameBuffer->AddRenderBuffer(renderBuffer);

		m_PostProcessReady = m_ScreenFrameBuffer->Check();
		if (!m_PostProcessReady)
		{
			ERROR_CORE("Post-processing framebuffer failed, falling back to direct rendering");
		}
		m_ScreenFrameBuffer->Unbind();
	}

	unsigned int fb;

	void OnUpdate(Timestep ts) override
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

	bool m_DbgDisableNormalMapping = false;
	float m_DbgPPOffset = 0.003f;
	int m_DbgPPEffect = 0;

	virtual void OnImGuiRender() override
	{
		auto& dl = m_ScnLight.dirLight;
		auto& pl = m_ScnLight.pointLights[0];
		auto& sl = m_ScnLight.spotLights[0];

		ImGui::Begin("Settings");

		ImGui::SliderFloat3("Dir light direction", glm::value_ptr(dl.direction), -1.f, 0.f);
		ImGui::ColorEdit3("Dir light ambient", glm::value_ptr(dl.ambient));
		ImGui::ColorEdit3("Dir light diffuse", glm::value_ptr(dl.diffuse));
		ImGui::ColorEdit3("Dir light specular", glm::value_ptr(dl.specular));

		ImGui::SliderFloat3("Point light position", glm::value_ptr(pl.position), -1500.f, 1500.f);
		ImGui::ColorEdit3("Point light ambient", glm::value_ptr(pl.ambient));
		ImGui::ColorEdit3("Point light diffuse", glm::value_ptr(pl.diffuse));
		ImGui::ColorEdit3("Point light specular", glm::value_ptr(pl.specular));
		ImGui::SliderFloat("Point light constant", &pl.constant, 0.f, 1.f);
		ImGui::SliderFloat("Point light linear", &pl.linear, 0.f, 0.001f);
		ImGui::SliderFloat("Point light quadratic", &pl.quadratic, 0.f, 0.001f);

		ImGui::ColorEdit3("Spot light ambient", glm::value_ptr(sl.ambient));
		ImGui::ColorEdit3("Spot light diffuse", glm::value_ptr(sl.diffuse));
		ImGui::ColorEdit3("Spot light specular", glm::value_ptr(sl.specular));
		ImGui::SliderFloat("Spot light constant", &sl.constant, 0.f, 1.f);
		ImGui::SliderFloat("Spot light linear", &sl.linear, 0.f, 0.001f);
		ImGui::SliderFloat("Spot light quadratic", &sl.quadratic, 0.f, 0.001f);

		ImGui::Checkbox("Disable normal mapping", &m_DbgDisableNormalMapping);
		ImGui::Checkbox("Enable post processing", &m_EnablePostProcessing);

		ImGui::SliderFloat("Post proc offset", &m_DbgPPOffset, 0.f, 0.01f);
		ImGui::SliderInt("Post proc effect", &m_DbgPPEffect, 0, 3);

		ImGui::End();
	}

	void AnimatePointLight(Timestep ts)
	{
		constexpr float max_x = 1100.f;
		constexpr float max_z = 150.f;
		constexpr float y = 200.f;
		constexpr float speed_x = 100.f;
		constexpr float speed_z = 50.f;

		static int direction_x = 1;
		static int direction_z = 1;
		auto& pos = m_ScnLight.pointLights[0].position;
		pos.y = y;

		pos.x += direction_x * speed_x * ts;
		if (abs(pos.x) > max_x) {
			pos.x = max_x * direction_x;
			direction_x *= -1;
		}

		pos.z += direction_z * speed_z * ts;
		if (abs(pos.z) > max_z) {
			pos.z = max_z * direction_z;
			direction_z *= -1;
		}
	}

	void OnEvent(Event& event) override
	{
	}

	void RenderScene()
	{
		auto defaultShader = AssetManager::GetShader(m_DefaultShader);
		defaultShader->Bind();
		defaultShader->UploadUniformsDefaultLighting(m_ScnLight, m_Camera.GetPosition());
		defaultShader->UploadUniformInt("u_dbgDisableNormalMapping", m_DbgDisableNormalMapping ? 1 : 0);

		m_Model->SetTransform(glm::mat4(1.f));
		m_Model->Render(defaultShader);

		RenderCommand::CullFaces(false);
		auto skyboxShader = AssetManager::GetShader(m_ScyboxShader);
		m_Skybox->Render(skyboxShader);
		RenderCommand::CullFaces(true);

		auto lightShader = AssetManager::GetShader(m_LightSourceShader);
		for (const auto& ls : m_LightSources)
			ls->Render(lightShader);
	}

private:
	SceneLight m_ScnLight;
	SPtr<Scn::Model> m_Model;

	std::vector<SPtr<Scn::Cube>> m_LightSources;

	SPtr<Scn::SkyBox> m_Skybox = std::make_shared<Scn::SkyBox>();
	SPtr<Scn::Quad> m_ScreenQuad = std::make_shared<Scn::Quad>(glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 0.f)));

	std::string m_DefaultShader = "default";
	std::string m_LightSourceShader = "flat_color";
	std::string m_ScreenShader = "default_screen";
	std::string m_ScyboxShader = "default_skybox";

	SPtr<FrameBuffer> m_ScreenFrameBuffer;
	bool m_PostProcessReady = false;
	bool m_EnablePostProcessing = false;

	FlyCamera m_Camera;
};

class Sandbox : public Application
{
public:
	Sandbox()
	{
		PushLayer(new ExampleLayer((float)GetWindow().GetWidth(), (float)GetWindow().GetHeight()));
	}

	~Sandbox()
	{

	}

};

Engine::Application* Engine::CreateApplication()
{
	return new Sandbox();
}
