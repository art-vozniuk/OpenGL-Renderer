#pragma once

#include "pch.h"
#include "Renderer/Camera.h"
#include "Core/Timestep.h"
#include "glm/gtc/matrix_transform.hpp"

namespace Engine {

	class FlyCamera
	{
	public:
		FlyCamera(void) : m_Camera(MakeShared<Camera>()) { m_Camera->RecalculateViewMatrix(m_Transform); }
		FlyCamera(const SPtr<Camera>& camera) : m_Camera(camera) { m_Camera->RecalculateViewMatrix(m_Transform); }

		void SetPerspective(float fovy, float aspect, float zNear, float zFar) { m_Camera->SetPerspective(fovy, aspect, zNear, zFar, m_Transform); }

		void Update(Engine::Timestep ts);

		glm::vec3 GetPosition(void) { return glm::vec3(m_Transform[3]); }
		const glm::mat4& GetTransform(void) const { return m_Transform; }
		glm::mat4& GetTransform(void) { return m_Transform; }
		void SetTransform(const glm::mat4& transform);

		const SPtr<Camera>	GetRenderCamera(void) const { return m_Camera; }

		float m_MoveSpeed = 1.0f;
		float m_RotationSpeed = 0.13f;

	private:
		SPtr<Camera> m_Camera;
		glm::mat4 m_Transform = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.5f, 0.f));

		// Yaw / pitch are kept in sync with m_Transform so that first mouse-
		// drag after SetTransform doesn't snap the camera back to the old
		// static (-90°, 0°) default. Degrees; yaw in XZ plane, pitch around X.
		float m_Yaw   = -90.0f;
		float m_Pitch = 0.0f;
		bool  m_MouseWasPressed = false;
		std::pair<float, float> m_LastMousePos{ 0.0f, 0.0f };
	};

}
