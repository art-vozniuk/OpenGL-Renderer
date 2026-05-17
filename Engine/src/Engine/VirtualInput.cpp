#include "pch.h"
#include "VirtualInput.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace Engine {

	namespace {
		VirtualInputState g_state;
	}

	VirtualInputState& GetVirtualInput() { return g_state; }

	void ConsumeOrbitDeltas(float& outYaw, float& outPitch)
	{
		outYaw   = g_state.orbitYaw;
		outPitch = g_state.orbitPitch;
		g_state.orbitYaw   = 0.0f;
		g_state.orbitPitch = 0.0f;
	}

	float ConsumeZoomDelta()
	{
		const float d = g_state.zoomDelta;
		g_state.zoomDelta = 0.0f;
		return d;
	}

}


// ---------------------------------------------------------------------------
// JS-callable bridge. extern "C" + EMSCRIPTEN_KEEPALIVE keeps the symbols in
// the wasm export table so Module.ccall can find them at runtime. The
// matching CMake -sEXPORTED_FUNCTIONS list is in CMakeLists.txt.
//
// Native builds compile these as plain C ABI no-op-y functions; nothing in
// the engine calls them outside the JS bridge.
// ---------------------------------------------------------------------------

#ifdef __EMSCRIPTEN__
#define VINPUT_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define VINPUT_EXPORT
#endif


extern "C" {

	VINPUT_EXPORT void vinput_apply_orbit(float yawDelta, float pitchDelta)
	{
		auto& s = Engine::GetVirtualInput();
		s.orbitYaw   += yawDelta;
		s.orbitPitch += pitchDelta;
	}

	VINPUT_EXPORT void vinput_apply_zoom(float delta)
	{
		auto& s = Engine::GetVirtualInput();
		s.zoomDelta += delta;
	}

}
