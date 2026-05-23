#include "pch.h"
#include "FlyCamera.h"
#include "Input.h"
#include "KeyCodes.h"

#include <cmath>

namespace Engine {

	namespace {
		// Yaw + pitch (degrees) → unit forward vector.
		// Convention matches old code: (yaw=-90, pitch=0) → (0, 0, -1).
		glm::vec3 ForwardFromAngles(float yawDeg, float pitchDeg) {
			const float cy = std::cos(glm::radians(yawDeg));
			const float sy = std::sin(glm::radians(yawDeg));
			const float cp = std::cos(glm::radians(pitchDeg));
			const float sp = std::sin(glm::radians(pitchDeg));
			return glm::normalize(glm::vec3(cy * cp, sp, sy * cp));
		}
	}

	void FlyCamera::SetPose(const glm::vec3& position, const glm::vec3& forward)
	{
		m_Position = position;
		m_Velocity = glm::vec3(0.0f);

		const glm::vec3 f = glm::length(forward) > 1e-6f
		    ? glm::normalize(forward)
		    : glm::vec3(0.0f, 0.0f, -1.0f);

		m_Pitch = glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f)));
		m_Yaw   = glm::degrees(std::atan2(f.z, f.x));

		Rebuild();
	}


	glm::vec3 FlyCamera::GetForward() const
	{
		return ForwardFromAngles(m_Yaw, m_Pitch);
	}


	void FlyCamera::Update(Timestep ts)
	{
		// --- Mouse look (instant; no smoothing). ----------------------------
		const bool mouseHeld = Input::IsMouseButtonPressed(m_DragButton);
		if (mouseHeld) {
			const auto pos = Input::GetMousePosition();
			if (!m_MouseDragging) {
				m_MouseDragging = true;
				m_LastMouse = pos;
			} else {
				// Drag right (positive dx) → camera turns right.
				m_Yaw   += m_RotationSpeed * (pos.first  - m_LastMouse.first );
				m_Pitch -= m_RotationSpeed * (pos.second - m_LastMouse.second);
				m_LastMouse = pos;
			}
		} else {
			m_MouseDragging = false;
		}
		m_Pitch = glm::clamp(m_Pitch, -89.0f, 89.0f);

		const glm::vec3 fwd   = ForwardFromAngles(m_Yaw, m_Pitch);
		const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
		// WASD travel along XZ plane (independent of pitch), Q/E along world Y.
		const glm::vec3 fwdXZ = glm::normalize(glm::vec3(fwd.x, 0.0f, fwd.z));

		// --- Target velocity from currently-held keys. ----------------------
		glm::vec3 wish(0.0f);
		if (Input::IsKeyPressed(KEY_W)) wish += fwdXZ;
		if (Input::IsKeyPressed(KEY_S)) wish -= fwdXZ;
		if (Input::IsKeyPressed(KEY_D)) wish += right;
		if (Input::IsKeyPressed(KEY_A)) wish -= right;
		if (Input::IsKeyPressed(KEY_E)) wish += glm::vec3(0.0f, 1.0f, 0.0f);
		if (Input::IsKeyPressed(KEY_Q)) wish -= glm::vec3(0.0f, 1.0f, 0.0f);
		if (glm::length(wish) > 0.0f) {
			wish = glm::normalize(wish) * m_MaxMoveSpeed;
		}

		// --- Ease velocity toward target (critically-damped, fps-independent).
		// k = 1 - exp(-rate*dt) gives the same time-constant regardless of
		// frame rate. Accel up AND coast down both run through this lerp,
		// so releasing keys glides to a stop instead of slamming to zero.
		const float k = 1.0f - std::exp(-m_Accel * static_cast<float>(ts));
		m_Velocity = glm::mix(m_Velocity, wish, k);

		// Zero out micro-velocity below the noise floor — otherwise the
		// camera keeps drifting at sub-pixel rates and never quite stops.
		if (glm::length(m_Velocity) < 0.001f) m_Velocity = glm::vec3(0.0f);

		m_Position += m_Velocity * static_cast<float>(ts);

		Rebuild();
	}


	void FlyCamera::Rebuild()
	{
		const glm::vec3 fwd = ForwardFromAngles(m_Yaw, m_Pitch);
		m_Transform = glm::inverse(
		    glm::lookAt(m_Position, m_Position + fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
		m_Camera->RecalculateViewMatrix(m_Transform);
	}


	PoseSnapshot FlyCamera::Snapshot() const
	{
		PoseSnapshot s;
		s.position    = m_Position;
		s.forward     = GetForward();
		// We don't track an orbit target/radius. Provide a sensible
		// default in case the next controller is an orbit camera:
		// pivot a fixed distance in front of the eye along forward.
		s.orbitRadius = 3.0f;
		s.orbitTarget = m_Position + s.forward * s.orbitRadius;
		return s;
	}


	void FlyCamera::ApplySnapshot(const PoseSnapshot& s)
	{
		SetPose(s.position, s.forward);
	}

}
