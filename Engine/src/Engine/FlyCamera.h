#pragma once

#include "pch.h"
#include "Renderer/Camera.h"
#include "Core/Timestep.h"
#include "glm/gtc/matrix_transform.hpp"

namespace Engine {

	/*
	 * FlyCamera
	 * =========
	 * Keyboard-driven free-fly camera with velocity smoothing — pressing
	 * W/A/S/D/Q/E eases the camera toward a target velocity, releasing
	 * eases it back to zero. Movement never cuts off abruptly; the rate
	 * constant `m_Accel` controls how snappy the ramp is.
	 *
	 * Mouse-look is intentionally instant: rotation feels worst when
	 * smoothed (input lag), best when 1:1.
	 *
	 * Activated by the scene when the user presses any WASD key while in
	 * orbit mode, or clicks the "fly" toggle in the toolbar. Disabled on
	 * touch devices (no on-screen joystick).
	 */
	class FlyCamera
	{
	public:
		FlyCamera(void) : m_Camera(MakeShared<Camera>()) {}
		FlyCamera(const SPtr<Camera>& camera) : m_Camera(camera) {}

		void SetPerspective(float fovy, float aspect, float zNear, float zFar) {
			m_Camera->SetPerspective(fovy, aspect, zNear, zFar, m_Transform);
		}

		void Update(Engine::Timestep ts);

		// Drop into this pose without any momentum carryover. Called on
		// mode switch from orbit so the fly camera starts where the user
		// last was, looking the same direction.
		void SetPose(const glm::vec3& position, const glm::vec3& forward);

		glm::vec3 GetPosition(void) const { return m_Position; }
		glm::vec3 GetForward(void)  const;
		const glm::mat4& GetTransform(void) const { return m_Transform; }
		const SPtr<Camera> GetRenderCamera(void) const { return m_Camera; }

		// Tunables (units in comments).
		float m_MaxMoveSpeed   = 4.0f;   // world units / second at full tilt
		float m_Accel          = 6.0f;   // 1/s — rate constant for velocity easing
		float m_RotationSpeed  = 0.25f;  // degrees per pixel of mouse drag

	private:
		void Rebuild();

		SPtr<Camera> m_Camera;
		glm::mat4    m_Transform = glm::mat4(1.0f);

		glm::vec3 m_Position = glm::vec3(0.0f);
		glm::vec3 m_Velocity = glm::vec3(0.0f);

		float m_Yaw   = -90.0f;  // degrees; (yaw=-90, pitch=0) → forward = -Z
		float m_Pitch = 0.0f;

		bool                    m_MouseDragging = false;
		std::pair<float, float> m_LastMouse{0.0f, 0.0f};
	};

}
