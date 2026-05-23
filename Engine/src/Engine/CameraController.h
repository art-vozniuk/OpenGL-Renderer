#pragma once

#include "pch.h"
#include "Renderer/Camera.h"
#include "Core/Timestep.h"

#include <glm/glm.hpp>

namespace Engine {

	enum class CameraMode { Orbit = 0, Fly = 1 };

	/*
	 * PoseSnapshot
	 * ------------
	 * Mode-agnostic pose container used to transfer state between camera
	 * controllers when the scene swaps fly ↔ orbit. Each concrete
	 * controller's Snapshot()/ApplySnapshot() picks the fields it cares
	 * about, so a fly-to-orbit transition preserves the orbit radius
	 * that the orbit controller last knew about, and an orbit-to-fly
	 * transition starts the fly camera in the same pose the user was
	 * looking at.
	 */
	struct PoseSnapshot
	{
		glm::vec3 position    = glm::vec3(0.0f);
		glm::vec3 forward     = glm::vec3(0.0f, 0.0f, -1.0f);
		glm::vec3 orbitTarget = glm::vec3(0.0f);
		float     orbitRadius = 3.0f;
	};

	/*
	 * CameraController — abstract base for fly / orbit cameras.
	 *
	 * Why it exists: scenes used to keep BOTH a FlyCamera and an
	 * OrbitCamera member, with an `if (m_Mode == Orbit) ... else ...`
	 * branch around every call. That paid for itself when the two
	 * cameras shared no API surface at all — but they do (Update,
	 * GetRenderCamera, GetPosition, SetPerspective, SetDragButton). This
	 * interface makes the shared bits explicit; scenes hold a single
	 * unique_ptr<CameraController> and the mode swap is a pointer swap.
	 */
	class CameraController
	{
	public:
		virtual ~CameraController() = default;

		// Per-frame input read + state update. Rebuilds the underlying
		// render camera's view matrix.
		virtual void Update(Timestep ts) = 0;

		// The actual render-side Camera object that scene code hands to
		// Renderer::BeginScene().
		virtual const SPtr<Camera>& GetRenderCamera() const = 0;

		virtual glm::vec3 GetPosition() const = 0;
		virtual glm::vec3 GetForward()  const = 0;
		virtual const glm::mat4& GetTransform() const = 0;

		virtual void SetPerspective(float fovYRad, float aspect,
		                            float zNear,   float zFar) = 0;

		// Which mouse button drives drag-to-look. Editor scenes set RMB;
		// viewer scenes keep the default LMB.
		virtual void SetDragButton(int glfwButton) = 0;
		virtual int  GetDragButton() const = 0;

		// Snapshot the current pose so a SwitchCameraTo* can preserve it.
		// ApplySnapshot reads whichever fields are relevant to the concrete
		// controller (fly: position + forward; orbit: orbitTarget + position).
		virtual PoseSnapshot Snapshot() const = 0;
		virtual void         ApplySnapshot(const PoseSnapshot& s) = 0;

		virtual CameraMode Mode() const = 0;

		// True when the JS bridge's accumulated orbit/zoom deltas are
		// meaningful for this controller. Used by SceneBase to swallow
		// pending input before it pops on the next mode switch.
		virtual bool ConsumesOrbitInput() const = 0;
	};

}
