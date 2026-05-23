#pragma once

#include "Camera.h"
#include "WGPUContext.h"
#include "../Core.h"

#include <glm/glm.hpp>

namespace Engine {

	/*
	 * GridRenderer — infinite XZ-plane grid as a screen-space pass.
	 *
	 * Single fullscreen triangle; the fragment shader reconstructs the
	 * world-space ray from the inverse view-projection and intersects it
	 * with y=0 to produce the grid lines. No vertex buffer, no geometry —
	 * cheap to mix in alongside other render passes.
	 *
	 * Lives in Engine/ because it's a generic primitive: editor / mesh
	 * scenes will all want it. Scenes that don't (e.g. catalog gsplat
	 * viewer) just don't instantiate it.
	 */
	class GridRenderer
	{
	public:
		explicit GridRenderer(WGPUContext& ctx);
		~GridRenderer();

		GridRenderer(const GridRenderer&)            = delete;
		GridRenderer& operator=(const GridRenderer&) = delete;

		// Encode the grid draw into an already-open render pass. Updates
		// the uniform first (one queue write) then draws 3 vertices.
		void EncodeRender(WGPURenderPassEncoder pass,
		                  const SPtr<Camera>& camera);

		// Tunables — caller can override after construction. Defaults
		// match a "ground floor for a human-scale subject" look.
		float m_MinorSpacing = 1.0f;   // 1-unit minor lines
		float m_MajorStride  = 10.0f;  // major line every 10 minor
		float m_FadeDist     = 80.0f;  // grid disappears past 80 units
		float m_Thickness    = 1.0f;   // line thickness in pixels (≥1)

	private:
		void CreatePipeline();
		void DestroyGpuResources();

		WGPUContext* m_Ctx = nullptr;

		WGPUBuffer          m_Uniform = nullptr;
		WGPUBindGroupLayout m_BGL     = nullptr;
		WGPUBindGroup       m_BG      = nullptr;
		WGPUPipelineLayout  m_PL      = nullptr;
		WGPURenderPipeline  m_Pipe    = nullptr;
	};

}
