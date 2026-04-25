#pragma once

// Public Engine API. Re-exports just what app code needs.
//
// The pre-WebGPU port re-exported a deeper rendering surface (Buffer,
// VertexArray, Shader, Texture, RenderCommand). Those abstractions
// were OpenGL-shaped and have been removed. Scenes that need GPU access
// pull in Renderer/WGPUContext.h directly.

#include "Engine/Application.h"
#include "Engine/Layer.h"
#include "Engine/Log.h"

#include "Engine/Core/Timestep.h"

#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/MouseButtonCodes.h"

// ---Renderer------------------------
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/WGPUContext.h"
#include "Engine/Renderer/Camera.h"
// -----------------------------------

// ---Entry Point---------------------
#include "Engine/EntryPoint.h"
// -----------------------------------
