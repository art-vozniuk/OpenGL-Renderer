#pragma once

#include <glm/glm.hpp>

namespace Engine {

	/*
	 * VirtualInput
	 * ------------
	 * A bridge for non-keyboard, non-mouse input sources to drive the
	 * fly camera — currently used by the on-screen mobile joysticks
	 * injected into Sandbox.html.
	 *
	 * The on-screen UI lives in JavaScript and pokes this state every
	 * frame via Module.ccall (see patch_sandbox_html.py for the JS
	 * driver). The C++ side is read-only from FlyCamera.
	 *
	 * - move:   per-axis -1..1 (X = strafe, Y = up/down, Z = forward).
	 *           Held value: written once per frame from the JS poll
	 *           loop; FlyCamera scales by m_MoveSpeed * timestep.
	 * - look:   yaw / pitch deltas accumulated since last consume.
	 *           Reset to 0 by ConsumeLook() every frame after FlyCamera
	 *           applies them.
	 *
	 * Native builds never receive non-zero values (no JS bridge), so
	 * FlyCamera's existing keyboard/mouse path is unaffected.
	 */
	struct VirtualInputState
	{
		glm::vec3 move = glm::vec3(0.0f); // X strafe, Y up, Z forward
		float     lookYaw   = 0.0f;       // accumulated, in degrees
		float     lookPitch = 0.0f;       // accumulated, in degrees
	};

	VirtualInputState& GetVirtualInput();

	/*
	 * Snapshots the look deltas for FlyCamera and zeroes them so the
	 * next frame starts fresh. Move (held) is NOT zeroed — JS owns the
	 * "key down / key up" semantics by writing the latest joystick
	 * vector each frame (zero when released).
	 */
	void ConsumeLookDeltas(float& outYaw, float& outPitch);

}
