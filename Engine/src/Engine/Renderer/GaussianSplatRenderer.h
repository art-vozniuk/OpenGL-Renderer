#pragma once

#include "SplatLoader.h"
#include "Shader.h"
#include "Camera.h"
#include "../Core.h"

#include <cstdint>

namespace Engine {

	/*
	 * Stand-alone renderer for a single gaussian-splat scene.
	 *
	 * Owns:
	 *   - a shared quad VAO (4 corner vertices + 6 indices)
	 *   - four per-instance VBOs (SoA: positions, scales, rotations, colors)
	 *
	 * Rendering path is deliberately separate from the forward mesh renderer
	 * because the draw setup is fundamentally different: no depth write,
	 * premultiplied-alpha blending, and an instanced draw call that issues
	 * one quad per splat.
	 *
	 * The caller is responsible for setting / restoring GL state around
	 * Render(); the renderer itself just does what it needs for its own
	 * draw and puts blend/depth back to a sensible default on exit.
	 */
	class GaussianSplatRenderer
	{
	public:
		GaussianSplatRenderer();
		~GaussianSplatRenderer();

		GaussianSplatRenderer(const GaussianSplatRenderer&) = delete;
		GaussianSplatRenderer& operator=(const GaussianSplatRenderer&) = delete;

		// Reloads GPU buffers from a parsed splat dataset. Safe to call
		// multiple times (replaces previous data).
		void Upload(const SplatData& data);

		// Draws the splats using the provided camera. Expects the default
		// framebuffer to be bound; does NOT clear. Caller sets up the camera
		// matrices via the shader uniforms indirectly (this method uploads
		// them from the camera object).
		void Render(const SPtr<Camera>& camera, const glm::vec2& viewportSize);

		size_t SplatCount() const { return m_Count; }

	private:
		void CreateQuadMesh();
		void CreateInstanceBuffers();
		void DestroyGpuResources();

		// GL handles. Held as uint32_t to avoid leaking <glad>/<GLES3> from
		// this header into translation units that don't need them.
		uint32_t m_Vao = 0;
		uint32_t m_QuadVbo = 0;
		uint32_t m_QuadEbo = 0;

		uint32_t m_PosVbo   = 0;
		uint32_t m_ScaleVbo = 0;
		uint32_t m_RotVbo   = 0;
		uint32_t m_ColorVbo = 0;

		size_t m_Count = 0;
	};

}
