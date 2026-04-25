#pragma once

// Legacy interface — pre-WebGPU port we had OpenGL/WebGL behind this. Kept
// as an empty header so existing #include lines compile. The active GPU
// context is now `WGPUContext`, owned by the Application.

namespace Engine {

	// (intentionally empty; see Renderer/WGPUContext.h)

}
