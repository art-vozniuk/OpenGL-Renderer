#pragma once

#include <glm/glm.hpp>

namespace Engine {

	/*
	 * VirtualInput
	 * ------------
	 * Bridge for non-mouse / non-wheel pointer events to drive the
	 * OrbitCamera — currently used by the touch + pinch handlers injected
	 * into Sandbox.html.
	 *
	 * The JS overlay (see patch_sandbox_html.py) accumulates touch-drag
	 * yaw/pitch and pinch zoom each frame and forwards them via the C
	 * exports below. OrbitCamera::Update reads them through ConsumeOrbit /
	 * ConsumeZoom and zeroes the accumulators so the next frame starts
	 * fresh.
	 *
	 * Native builds never receive non-zero values (no JS bridge), so the
	 * mouse/wheel path in OrbitCamera is unaffected.
	 */
	struct VirtualInputState
	{
		float orbitYaw   = 0.0f;  // accumulated, in degrees
		float orbitPitch = 0.0f;  // accumulated, in degrees
		float zoomDelta  = 0.0f;  // accumulated, log-radius units (positive = zoom out)
	};

	VirtualInputState& GetVirtualInput();

	// Snapshot + zero the orbit / zoom accumulators. Called once per frame
	// by OrbitCamera::Update.
	void ConsumeOrbitDeltas(float& outYaw, float& outPitch);
	float ConsumeZoomDelta();

}
