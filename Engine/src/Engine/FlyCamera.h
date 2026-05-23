#pragma once

#include "pch.h"
#include "CameraController.h"
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
	 * Scenes interact with this through the CameraController base —
	 * direct calls to SetPose() are still available when concrete-typed
	 * access is convenient (e.g. inside SceneBase::SwitchCameraToFly).
	 */
	class FlyCamera : public CameraController
	{
	public:
		FlyCamera(void) : m_Camera(MakeShared<Camera>()) {}
		FlyCamera(const SPtr<Camera>& camera) : m_Camera(camera) {}

		// --- CameraController -----------------------------------------------
		void Update(Timestep ts) override;

		const SPtr<Camera>& GetRenderCamera() const override { return m_Camera; }
		glm::vec3 GetPosition() const override { return m_Position; }
		glm::vec3 GetForward()  const override;
		const glm::mat4& GetTransform() const override { return m_Transform; }

		void SetPerspective(float fovYRad, float aspect, float zNear, float zFar) override
		{
			m_Camera->SetPerspective(fovYRad, aspect, zNear, zFar, m_Transform);
		}

		void SetDragButton(int glfwButton) override { m_DragButton = glfwButton; }
		int  GetDragButton() const override { return m_DragButton; }

		PoseSnapshot Snapshot() const override;
		void         ApplySnapshot(const PoseSnapshot& s) override;

		CameraMode Mode() const override { return CameraMode::Fly; }
		bool       ConsumesOrbitInput() const override { return false; }

		// --- FlyCamera-specific ---------------------------------------------
		// Drop into this pose without any momentum carryover.
		void SetPose(const glm::vec3& position, const glm::vec3& forward);

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

		int                     m_DragButton    = 0; // GLFW MOUSE_BUTTON_LEFT
		bool                    m_MouseDragging = false;
		std::pair<float, float> m_LastMouse{0.0f, 0.0f};
	};

}
