#include "SceneBase.h"

#include "Engine/FlyCamera.h"
#include "Engine/OrbitCamera.h"
#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/VirtualInput.h"

#include <cstring>
#include <memory>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Sandbox {

	SceneBase::SceneBase(const std::string& id, float screenWidth, float screenHeight)
		: Engine::Layer(id)
		, m_Id(id)
		, m_ScreenWidth(screenWidth)
		, m_ScreenHeight(screenHeight)
	{
	}


	void SceneBase::SwitchCameraToOrbit(const glm::vec3& target, const glm::vec3& eye)
	{
		const bool wasFly = m_Camera && m_Camera->Mode() == Engine::CameraMode::Fly;

		auto cam = std::make_unique<Engine::OrbitCamera>();
		const float aspect = (m_ScreenHeight > 0.0f) ? (m_ScreenWidth / m_ScreenHeight) : 1.0f;
		cam->SetPerspective(m_CameraConfig.fovYRad, aspect,
		                    m_CameraConfig.zNear, m_CameraConfig.zFar);
		cam->SetDragButton(m_CameraConfig.dragButton);
		cam->SetOrbit(target, eye);

		m_Camera = std::move(cam);

		// Only emit the message on an actual mode change so scenes that
		// re-init the camera at the same mode don't spam the parent UI.
		if (wasFly) {
			PostSceneMessage("{\"type\":\"camera-mode-changed\",\"mode\":\"orbit\"}");
		}
	}


	void SceneBase::SwitchCameraToFly(const glm::vec3& position, const glm::vec3& forward)
	{
		const bool wasOrbit = m_Camera && m_Camera->Mode() == Engine::CameraMode::Orbit;

		auto cam = std::make_unique<Engine::FlyCamera>();
		const float aspect = (m_ScreenHeight > 0.0f) ? (m_ScreenWidth / m_ScreenHeight) : 1.0f;
		cam->SetPerspective(m_CameraConfig.fovYRad, aspect,
		                    m_CameraConfig.zNear, m_CameraConfig.zFar);
		cam->SetDragButton(m_CameraConfig.dragButton);
		cam->m_MaxMoveSpeed = m_CameraConfig.flyMaxSpeed;
		cam->SetPose(position, forward);

		m_Camera = std::move(cam);

		if (wasOrbit) {
			PostSceneMessage("{\"type\":\"camera-mode-changed\",\"mode\":\"fly\"}");
		}
	}


	const char* SceneBase::HandleStandardCameraArbitration(bool autoFlipOnFlyKey)
	{
		const int requested = Engine::ConsumeRequestedMode();
		const Engine::CameraMode current = m_Camera->Mode();

		auto poseFromCurrent = [&]() {
			return m_Camera->Snapshot();
		};

		if (requested == 0 && current != Engine::CameraMode::Orbit) {
			const Engine::PoseSnapshot s = poseFromCurrent();
			SwitchCameraToOrbit(s.orbitTarget, s.position);
			return "orbit";
		}
		if (requested == 1 && current != Engine::CameraMode::Fly) {
			const Engine::PoseSnapshot s = poseFromCurrent();
			SwitchCameraToFly(s.position, s.forward);
			return "fly";
		}

		if (autoFlipOnFlyKey
		    && current == Engine::CameraMode::Orbit
		    && AnyFlyKeyPressed())
		{
			const Engine::PoseSnapshot s = poseFromCurrent();
			SwitchCameraToFly(s.position, s.forward);
			return "fly";
		}

		return nullptr;
	}


	void SceneBase::DrainUnusedOrbitInput()
	{
		if (!m_Camera) return;
		if (m_Camera->ConsumesOrbitInput()) return;
		float dy, dp;
		Engine::ConsumeOrbitDeltas(dy, dp);
		(void)Engine::ConsumeZoomDelta();
	}


	bool SceneBase::AnyFlyKeyPressed()
	{
		return Engine::Input::IsKeyPressed(KEY_W)
		    || Engine::Input::IsKeyPressed(KEY_A)
		    || Engine::Input::IsKeyPressed(KEY_S)
		    || Engine::Input::IsKeyPressed(KEY_D)
		    || Engine::Input::IsKeyPressed(KEY_Q)
		    || Engine::Input::IsKeyPressed(KEY_E);
	}


	void SceneBase::PostSceneMessage(const char* json)
	{
	#ifdef __EMSCRIPTEN__
		EM_ASM({
			try {
				if (typeof window !== 'undefined' && window.parent !== window) {
					window.parent.postMessage(JSON.parse(UTF8ToString($0)), '*');
				}
			} catch (e) {}
		}, json);
	#else
		(void)json;
	#endif
	}

}
