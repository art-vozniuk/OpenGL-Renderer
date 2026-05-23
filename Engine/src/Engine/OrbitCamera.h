#pragma once

#include "pch.h"
#include "CameraController.h"
#include "Renderer/Camera.h"
#include "Core/Timestep.h"
#include "glm/gtc/matrix_transform.hpp"

namespace Engine {

	/*
	 * OrbitCamera
	 * ===========
	 * Touch / mouse / pinch driven orbit camera with soft-elastic limits.
	 *
	 * State is (target, yaw, pitch, radius) — the camera always sits on a
	 * sphere centered at `target`, looking at it. Yaw / pitch / radius
	 * each have a "rest" value that the camera springs back to whenever
	 * the user lets go. Pushing past the soft limits scales the input
	 * delta down so it feels like a rubber band — keeps users inside the
	 * good-quality reconstruction window without slamming into a hard wall.
	 *
	 * Input sources (all combined per frame):
	 *   - Mouse: drag (button configurable) = orbit, scroll wheel = zoom.
	 *   - VirtualInput JS bridge: touch-drag → orbit deltas,
	 *     pinch → zoom delta. See VirtualInput.h.
	 */
	class OrbitCamera : public CameraController
	{
	public:
		OrbitCamera(void) : m_Camera(MakeShared<Camera>()) { Rebuild(); }
		OrbitCamera(const SPtr<Camera>& camera) : m_Camera(camera) { Rebuild(); }

		// --- CameraController -----------------------------------------------
		void Update(Timestep ts) override;

		const SPtr<Camera>& GetRenderCamera() const override { return m_Camera; }
		glm::vec3 GetPosition() const override { return glm::vec3(m_Transform[3]); }
		glm::vec3 GetForward()  const override { return glm::normalize(m_Target - GetPosition()); }
		const glm::mat4& GetTransform() const override { return m_Transform; }

		void SetPerspective(float fovYRad, float aspect, float zNear, float zFar) override
		{
			m_Camera->SetPerspective(fovYRad, aspect, zNear, zFar, m_Transform);
		}

		void SetDragButton(int glfwButton) override { m_DragButton = glfwButton; }
		int  GetDragButton() const override { return m_DragButton; }

		PoseSnapshot Snapshot() const override;
		void         ApplySnapshot(const PoseSnapshot& s) override;

		CameraMode Mode() const override { return CameraMode::Orbit; }
		bool       ConsumesOrbitInput() const override { return true; }

		// --- OrbitCamera-specific -------------------------------------------
		// Initial pose: orbit `target` from `eye`. The (eye - target) offset
		// determines radius + initial yaw/pitch; those become the "rest"
		// values the elastic springs back to.
		void SetOrbit(const glm::vec3& target, const glm::vec3& eye);

		glm::vec3 GetTarget() const { return m_Target; }
		float     GetRadius() const { return m_Radius; }

		// Tunables — caller can override after construction.
		float m_DragSensitivity  = 0.25f;
		float m_WheelSensitivity = 0.10f;

		// Soft-elastic limits.
		float m_YawSoftLimit   = 30.0f;
		float m_PitchSoftLimit = 20.0f;
		float m_RadiusSoftMin  = 0.5f;
		float m_RadiusSoftMax  = 3.0f;
		float m_SpringRate     = 6.0f;

	private:
		void Rebuild();
		static float ElasticResistance(float excursion, float softLimit);

		SPtr<Camera> m_Camera;
		glm::mat4    m_Transform = glm::mat4(1.0f);

		glm::vec3 m_Target  = glm::vec3(0.0f);
		float     m_Radius  = 3.0f;
		float     m_Yaw     = 0.0f;
		float     m_Pitch   = 0.0f;

		glm::vec3 m_Target0 = glm::vec3(0.0f);
		float     m_Radius0 = 3.0f;
		float     m_Yaw0    = 0.0f;
		float     m_Pitch0  = 0.0f;

		int                     m_DragButton    = 0;
		bool                    m_MouseDragging = false;
		std::pair<float, float> m_LastMouse{0.0f, 0.0f};
	};

}
