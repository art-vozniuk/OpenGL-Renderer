#include "pch.h"
#include "FlyCamera.h"
#include "Input.h"
#include "KeyCodes.h"

#ifdef __EMSCRIPTEN__
constexpr float maxMoveSpeed = 1.f;
#else
constexpr float maxMoveSpeed = 5000.f;
#endif

namespace Engine {


	void FlyCamera::SetTransform(const glm::mat4& transform)
	{
		m_Transform = transform;
		m_Camera->RecalculateViewMatrix(transform);

		// Keep yaw/pitch in sync with the new transform — otherwise the
		// first mouse-drag after SetTransform would snap the camera to the
		// old static (-90°, 0°) convention.
		// Forward in world = camera-local -Z mapped via the transform's 3rd
		// column. Pitch = asin(fwd.y), yaw = atan2(fwd.z, fwd.x) [degrees].
		const glm::vec3 fwd = -glm::vec3(transform[2]);
		m_Pitch = glm::degrees(std::asin(glm::clamp(fwd.y, -1.0f, 1.0f)));
		m_Yaw   = glm::degrees(std::atan2(fwd.z, fwd.x));
	}


	void FlyCamera::Update(Timestep ts)
	{
		if (!Input::IsMouseButtonPressed(0)) {
			m_MouseWasPressed = false;
			return;
		}

		if (!m_MouseWasPressed) {
			m_MouseWasPressed = true;
			m_LastMousePos = Input::GetMousePosition();
			return;
		}

		m_MoveSpeed += Input::GetScroll().second * 10.f;
		m_MoveSpeed = glm::clamp(m_MoveSpeed, 0.1f, maxMoveSpeed);

		const auto newMousePos = Input::GetMousePosition();
		m_Yaw   -= m_RotationSpeed * (m_LastMousePos.first  - newMousePos.first);
		m_Pitch += m_RotationSpeed * (m_LastMousePos.second - newMousePos.second);
		m_Pitch  = glm::clamp(m_Pitch, -89.f, 89.f);

		m_LastMousePos = newMousePos;

		glm::vec3 frw(
			std::cos(glm::radians(m_Yaw)) * std::cos(glm::radians(m_Pitch)),
			std::sin(glm::radians(m_Pitch)),
			std::sin(glm::radians(m_Yaw)) * std::cos(glm::radians(m_Pitch))
		);
		frw = glm::normalize(frw);

		glm::vec3 pos   = GetPosition();
		glm::vec3 unitY = glm::vec3(0.f, 1.f, 0.f);
		glm::vec3 right = glm::normalize(glm::cross(frw, unitY));
		glm::vec3 up    = glm::normalize(glm::cross(right, frw));

		if (Input::IsKeyPressed(KEY_A)) pos -= right * (m_MoveSpeed * ts);
		else if (Input::IsKeyPressed(KEY_D)) pos += right * (m_MoveSpeed * ts);

		// XZ-plane horizontal movement (WASD independent of pitch).
		glm::vec3 frwXZ = glm::normalize(glm::vec3(frw.x, 0.f, frw.z));
		if (Input::IsKeyPressed(KEY_W)) pos += frwXZ * (m_MoveSpeed * ts);
		else if (Input::IsKeyPressed(KEY_S)) pos -= frwXZ * (m_MoveSpeed * ts);

		if (Input::IsKeyPressed(KEY_E)) pos += up * (m_MoveSpeed * ts);
		else if (Input::IsKeyPressed(KEY_Q)) pos -= up * (m_MoveSpeed * ts);

		// Build transform directly rather than round-tripping through
		// SetTransform — we already know yaw/pitch, no need to re-derive.
		m_Transform = glm::inverse(glm::lookAt(pos, pos + frw, unitY));
		m_Camera->RecalculateViewMatrix(m_Transform);
	}

}
