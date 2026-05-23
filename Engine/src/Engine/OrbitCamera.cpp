#include "pch.h"
#include "OrbitCamera.h"
#include "Input.h"
#include "VirtualInput.h"

#include <algorithm>
#include <cmath>

namespace Engine {

	void OrbitCamera::SetOrbit(const glm::vec3& target, const glm::vec3& eye)
	{
		const glm::vec3 offset = eye - target;
		const float radius = glm::length(offset);
		// Degenerate (eye == target) — pick a sane default radius so the
		// camera doesn't sit on top of the subject.
		const float r = (radius > 1e-4f) ? radius : 3.0f;

		const glm::vec3 n = (radius > 1e-4f) ? (offset / radius) : glm::vec3(0.0f, 0.0f, 1.0f);
		// Pitch = elevation above XZ plane; yaw = azimuth around Y, measured
		// so that (0, *, +Z) maps to yaw=0 — matches OpenGL right-handed
		// convention with -Z forward.
		const float pitch = glm::degrees(std::asin(glm::clamp(n.y, -1.0f, 1.0f)));
		const float yaw   = glm::degrees(std::atan2(n.x, n.z));

		m_Target  = target;
		m_Target0 = target;
		m_Radius  = r;
		m_Radius0 = r;
		m_Yaw     = yaw;
		m_Yaw0    = yaw;
		m_Pitch   = pitch;
		m_Pitch0  = pitch;

		Rebuild();
	}


	float OrbitCamera::ElasticResistance(float excursion, float softLimit)
	{
		// |x| <= softLimit → resistance ~ 1.0 (full effect).
		// |x| >> softLimit → resistance → 0 (rubber band tightens).
		// Quadratic falloff is gentle near rest and asymptotic far out.
		if (softLimit <= 0.0f) return 1.0f;
		const float n = excursion / softLimit;
		return 1.0f / (1.0f + n * n);
	}


	void OrbitCamera::Update(Timestep ts)
	{
		float dYaw   = 0.0f;
		float dPitch = 0.0f;
		float dZoom  = 0.0f;  // positive = zoom out, negative = zoom in

		// --- Desktop mouse: drag-to-orbit (button configurable), scroll = zoom.
		const bool mouseHeld = Input::IsMouseButtonPressed(m_DragButton);
		if (mouseHeld) {
			const auto pos = Input::GetMousePosition();
			if (!m_MouseDragging) {
				m_MouseDragging = true;
				m_LastMouse = pos;
			} else {
				// Drag right (positive dx) → camera moves LEFT around the
				// subject → subject visually rotates RIGHT. Feels like direct
				// manipulation of the scene, not the camera.
				dYaw   += (pos.first  - m_LastMouse.first ) * m_DragSensitivity;
				dPitch += (pos.second - m_LastMouse.second) * m_DragSensitivity;
				m_LastMouse = pos;
			}
		} else {
			m_MouseDragging = false;
		}
		const float scrollY = Input::GetScroll().second;
		if (scrollY != 0.0f) {
			// Scroll up → zoom in (subject grows), so flip the sign so that
			// positive `dZoom` means zoom-out.
			dZoom -= scrollY * m_WheelSensitivity;
		}

		// --- JS bridge (touch + pinch). --------------------------------------
		float jsYaw = 0.0f, jsPitch = 0.0f;
		ConsumeOrbitDeltas(jsYaw, jsPitch);
		dYaw   += jsYaw;
		dPitch += jsPitch;
		dZoom  += ConsumeZoomDelta();

		// --- Apply with rubber-band resistance past soft limits. -------------
		const bool userActive = mouseHeld
		    || dYaw != 0.0f || dPitch != 0.0f || dZoom != 0.0f;

		if (dYaw != 0.0f) {
			const float k = ElasticResistance(m_Yaw - m_Yaw0, m_YawSoftLimit);
			m_Yaw += dYaw * k;
		}
		if (dPitch != 0.0f) {
			const float k = ElasticResistance(m_Pitch - m_Pitch0, m_PitchSoftLimit);
			m_Pitch += dPitch * k;
			// Hard clamp at the poles to keep the orbit math from flipping.
			m_Pitch = glm::clamp(m_Pitch, -89.0f, 89.0f);
		}
		if (dZoom != 0.0f) {
			// Exponential zoom so the same scroll step feels the same near
			// and far. Then resist past the soft min/max radius bounds.
			const float zoomFactor = std::exp(dZoom);
			const float wanted = m_Radius * zoomFactor;
			const float minR = m_Radius0 * m_RadiusSoftMin;
			const float maxR = m_Radius0 * m_RadiusSoftMax;
			// excursion in log-space so resistance feels symmetric in/out.
			const float restLog = std::log(m_Radius0);
			const float curLog  = std::log(m_Radius);
			const float limLog  = std::log(wanted < m_Radius0 ? minR / m_Radius0
			                                                  : maxR / m_Radius0);
			const float softLim = std::abs(limLog);
			const float k = ElasticResistance(curLog - restLog, softLim);
			m_Radius *= std::exp(std::log(zoomFactor) * k);
			// Guard against catastrophic zoom-in into NaN territory.
			m_Radius = std::max(m_Radius, 0.001f);
		}

		// --- Spring back to rest when idle. ----------------------------------
		// 1 - exp(-rate * dt) gives a frame-rate-independent ease toward rest.
		if (!userActive) {
			const float kSpring = 1.0f - std::exp(-m_SpringRate * (float)ts);
			m_Yaw    = glm::mix(m_Yaw,    m_Yaw0,    kSpring);
			m_Pitch  = glm::mix(m_Pitch,  m_Pitch0,  kSpring);
			m_Radius = glm::mix(m_Radius, m_Radius0, kSpring);
		}

		Rebuild();
	}


	void OrbitCamera::Rebuild()
	{
		const float cy = std::cos(glm::radians(m_Yaw));
		const float sy = std::sin(glm::radians(m_Yaw));
		const float cp = std::cos(glm::radians(m_Pitch));
		const float sp = std::sin(glm::radians(m_Pitch));

		// Offset matches SetOrbit's inverse: yaw=0, pitch=0 → +Z direction.
		const glm::vec3 offset = m_Radius * glm::vec3(cp * sy, sp, cp * cy);
		const glm::vec3 eye    = m_Target + offset;

		m_Transform = glm::inverse(
		    glm::lookAt(eye, m_Target, glm::vec3(0.0f, 1.0f, 0.0f)));
		m_Camera->RecalculateViewMatrix(m_Transform);
	}


	PoseSnapshot OrbitCamera::Snapshot() const
	{
		PoseSnapshot s;
		s.position    = GetPosition();
		s.forward     = glm::normalize(m_Target - s.position);
		s.orbitTarget = m_Target;
		s.orbitRadius = m_Radius;
		return s;
	}


	void OrbitCamera::ApplySnapshot(const PoseSnapshot& s)
	{
		// SetOrbit re-derives radius from |eye - target|, so the orbit
		// radius in the snapshot is already encoded in the position/
		// target pair. We don't have to pass it explicitly.
		SetOrbit(s.orbitTarget, s.position);
	}

}
