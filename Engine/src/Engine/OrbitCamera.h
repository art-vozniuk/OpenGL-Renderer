#pragma once

#include "pch.h"
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
	 *   - Mouse: LMB-drag = orbit, scroll wheel = zoom.
	 *   - VirtualInput JS bridge: touch-drag → orbit deltas,
	 *     pinch → zoom delta. See VirtualInput.h.
	 *
	 * No keyboard, no panning, no roll. Single subject inspector.
	 */
	class OrbitCamera
	{
	public:
		OrbitCamera(void) : m_Camera(MakeShared<Camera>()) { Rebuild(); }
		OrbitCamera(const SPtr<Camera>& camera) : m_Camera(camera) { Rebuild(); }

		void SetPerspective(float fovy, float aspect, float zNear, float zFar) {
			m_Camera->SetPerspective(fovy, aspect, zNear, zFar, m_Transform);
		}

		void Update(Engine::Timestep ts);

		// Initial pose: orbit `target` from `eye`. The (eye - target) offset
		// determines radius + initial yaw/pitch; those become the "rest"
		// values the elastic springs back to.
		void SetOrbit(const glm::vec3& target, const glm::vec3& eye);

		glm::vec3 GetPosition(void) const { return glm::vec3(m_Transform[3]); }
		glm::vec3 GetTarget(void)   const { return m_Target; }
		float     GetRadius(void)   const { return m_Radius; }
		const glm::mat4& GetTransform(void) const { return m_Transform; }
		const SPtr<Camera> GetRenderCamera(void) const { return m_Camera; }

		// Tunables — caller can override after construction.
		float m_DragSensitivity = 0.25f;   // degrees per pixel
		float m_WheelSensitivity = 0.10f;  // unitless zoom step per scroll tick

		// Soft-elastic limits (rest ± this many degrees / × this radius).
		// Drag/zoom past the soft limit is increasingly resisted; release
		// snaps back via SpringRate-driven critical damping.
		float m_YawSoftLimit   = 30.0f;
		float m_PitchSoftLimit = 20.0f;
		float m_RadiusSoftMin  = 0.5f;     // multiplier of radius0
		float m_RadiusSoftMax  = 3.0f;     // multiplier of radius0
		float m_SpringRate     = 6.0f;     // higher = snappier return-to-rest

	private:
		void Rebuild();
		// Returns a 0..1 scale for an incoming delta given how far we
		// already are from rest. 1 at rest; smoothly tapers to 0 as the
		// excursion grows past the soft limit.
		static float ElasticResistance(float excursion, float softLimit);

		SPtr<Camera> m_Camera;
		glm::mat4    m_Transform = glm::mat4(1.0f);

		glm::vec3 m_Target  = glm::vec3(0.0f);
		float     m_Radius  = 3.0f;
		float     m_Yaw     = 0.0f;     // degrees; orbit angle around world Y
		float     m_Pitch   = 0.0f;     // degrees; elevation from XZ plane

		// Rest values (the auto-framed pose); elastic pulls back to these.
		glm::vec3 m_Target0 = glm::vec3(0.0f);
		float     m_Radius0 = 3.0f;
		float     m_Yaw0    = 0.0f;
		float     m_Pitch0  = 0.0f;

		// Mouse-drag bookkeeping.
		bool                    m_MouseDragging = false;
		std::pair<float, float> m_LastMouse{0.0f, 0.0f};
	};

}
