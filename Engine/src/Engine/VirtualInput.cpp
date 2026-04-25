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

	void ConsumeLookDeltas(float& outYaw, float& outPitch)
	{
		outYaw   = g_state.lookYaw;
		outPitch = g_state.lookPitch;
		g_state.lookYaw   = 0.0f;
		g_state.lookPitch = 0.0f;
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

	VINPUT_EXPORT void vinput_set_move(float x, float y, float z)
	{
		auto& s = Engine::GetVirtualInput();
		s.move.x = x;
		s.move.y = y;
		s.move.z = z;
	}

	VINPUT_EXPORT void vinput_apply_look(float yawDelta, float pitchDelta)
	{
		auto& s = Engine::GetVirtualInput();
		s.lookYaw   += yawDelta;
		s.lookPitch += pitchDelta;
	}

}
