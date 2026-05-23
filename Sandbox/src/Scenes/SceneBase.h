#pragma once

#include "Engine/Layer.h"
#include "Engine/CameraController.h"

#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Sandbox {

	/*
	 * SceneBase
	 *
	 * Common lifecycle wrapper for every Sandbox scene. Owns the active
	 * camera (one of fly / orbit) as a CameraController, plus a small
	 * shared API for the things every scene used to copy-paste:
	 *
	 *   - SwitchCameraToOrbit / SwitchCameraToFly
	 *       Build a new camera controller, transfer the pose snapshot
	 *       from the previous one (if any), apply the scene's stock
	 *       perspective + drag-button config, and post a
	 *       'camera-mode-changed' message to the parent frame.
	 *
	 *   - HandleStandardCameraArbitration(autoFlipOnFlyKey)
	 *       Drain the JS bridge's requested-mode (orbit/fly toolbar
	 *       button) and, optionally, auto-flip orbit → fly when any
	 *       WASDEQ key is pressed. The same code used to live in every
	 *       scene's OnUpdate.
	 *
	 *   - PostSceneMessage(json)
	 *       Forward a JSON-string message to the parent frame on web
	 *       builds. No-op on native.
	 *
	 * Scenes participate in the normal Layer lifecycle and now hold the
	 * camera through the base class only — no per-scene OrbitCamera /
	 * FlyCamera members, no per-scene CameraMode enums.
	 */
	class SceneBase : public Engine::Layer
	{
	public:
		SceneBase(const std::string& id, float screenWidth, float screenHeight);
		~SceneBase() override = default;

		const std::string& Id() const { return m_Id; }

		// Convenience accessor; non-null after the first SwitchCameraTo*.
		Engine::CameraController* CameraCtrl() const { return m_Camera.get(); }
		const Engine::SPtr<Engine::Camera>& RenderCamera() const
		{
			return m_Camera->GetRenderCamera();
		}

	protected:
		// Stock perspective + drag-button config, applied to every new
		// camera spawned by SwitchCameraTo*. Scenes override these once
		// in their constructor before the initial camera switch.
		struct CameraConfig
		{
			float fovYRad     = glm::radians(45.0f);
			float zNear       = 0.1f;
			float zFar        = 10000.0f;
			int   dragButton  = 0;    // GLFW MOUSE_BUTTON_LEFT
			float flyMaxSpeed = 4.0f; // FlyCamera::m_MaxMoveSpeed override
		};
		CameraConfig m_CameraConfig;

		// Build and install a fresh OrbitCamera. If a camera already exists,
		// its pose snapshot is captured first so SetOrbit picks the right
		// target / radius. Posts 'camera-mode-changed' if the mode changed.
		void SwitchCameraToOrbit(const glm::vec3& target, const glm::vec3& eye);

		// Same shape for fly. The new fly camera starts at `position`
		// looking along `forward` (passed through to FlyCamera::SetPose).
		void SwitchCameraToFly(const glm::vec3& position, const glm::vec3& forward);

		// Drain the JS bridge's mode-request queue + (optionally) auto-flip
		// orbit → fly when any of WASDEQ is down. Returns the new mode
		// name (orbit/fly) if a switch happened this frame, else nullptr.
		const char* HandleStandardCameraArbitration(bool autoFlipOnFlyKey);

		// Per-frame swallow: if the active camera doesn't consume orbit
		// inputs (i.e. fly mode), drain accumulated VirtualInput deltas
		// so they don't pop on the next switch back to orbit. Call from
		// OnUpdate exactly once.
		void DrainUnusedOrbitInput();

		// Helpers shared by all scenes.
		static bool AnyFlyKeyPressed();

	public:
		// Public-static so anonymous-namespace helpers inside individual
		// scene .cpp files can post progress / state messages without
		// being members of SceneBase.
		static void PostSceneMessage(const char* json);

	protected:

	protected:
		std::string                                   m_Id;
		float                                         m_ScreenWidth  = 0.0f;
		float                                         m_ScreenHeight = 0.0f;
		std::unique_ptr<Engine::CameraController>     m_Camera;
	};

}
