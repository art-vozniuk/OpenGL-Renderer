#pragma once

#include "SplatLoader.h"
#include "Camera.h"
#include "WGPUContext.h"
#include "../Core.h"

#include <cstdint>
#include <vector>

namespace Engine {

	/*
	 * Stand-alone WebGPU renderer for a single Gaussian-splat scene.
	 *
	 * Owns:
	 *   - one shared 6-index quad (4 corner verts + 6 indices)
	 *   - four per-instance vertex buffers (SoA: positions, scales,
	 *     rotations, colors)
	 *   - a uniform buffer with view / projection / viewport
	 *   - one render pipeline (alpha-over blend, no depth, instanced
	 *     triangle-list draw of `m_Count` quads)
	 *
	 * Per-frame the caller hands us an already-open WGPURenderPassEncoder
	 * (the swap-chain colour pass) and we encode our draw into it. Set-up
	 * and tear-down (clear, present) happen in the surrounding Renderer
	 * code.
	 *
	 * This is the WebGPU port of the original GL renderer; the SH
	 * (.ply view-dependent) path is temporarily disabled — only the
	 * antimatter15 .splat flat-colour path is wired up here. Will be
	 * brought back once the GS scene is solid.
	 */
	class GaussianSplatRenderer
	{
	public:
		explicit GaussianSplatRenderer(WGPUContext& ctx);
		~GaussianSplatRenderer();

		GaussianSplatRenderer(const GaussianSplatRenderer&) = delete;
		GaussianSplatRenderer& operator=(const GaussianSplatRenderer&) = delete;

		// Reload GPU buffers from a parsed splat dataset. Idempotent.
		void Upload(const SplatData& data);

		// Encode a draw into the active render pass.
		// `pass` must be the pass opened by the renderer for this frame
		// (cleared, no depth, sized to `viewportSize`). Updates the camera
		// uniform buffer and issues the instanced draw.
		void Render(WGPURenderPassEncoder pass,
		            const SPtr<Camera>& camera,
		            const glm::vec2& viewportSize);

		size_t SplatCount() const { return m_Count; }

		// Force a back-to-front sort against the supplied view matrix.
		// Used at scene-start so frame 0 is already sorted instead of
		// rendering in file order until the next motion-stop.
		void SortNow(const glm::mat4& viewMatrix) { Sort(viewMatrix); }

		// Per-stage timing snapshot — same shape as the GL renderer's
		// stats so the UI can display them once we re-enable ImGui.
		struct PerfStats {
			float sortMs      = 0.0f;
			float reshuffleMs = 0.0f;
			float uploadMs    = 0.0f;
			float drawMs      = 0.0f;
		};

		PerfStats LastFrame() const { return m_LastFrame; }
		PerfStats MaxLast5s() const;

	private:
		void CreateQuadGeometry();
		void CreateInstanceBuffers();
		void CreateUniformBuffer();
		void CreatePipeline();
		void DestroyGpuResources();

		void Sort(const glm::mat4& viewMatrix);
		bool NeedsResort(const glm::mat4& viewMatrix) const;

		WGPUContext* m_Ctx = nullptr;

		// Geometry: 4 corner verts, 6 indices per quad. Reused per instance.
		WGPUBuffer m_QuadVerts = nullptr;
		WGPUBuffer m_QuadIndices = nullptr;

		// Per-instance attributes.
		WGPUBuffer m_PosBuf   = nullptr;
		WGPUBuffer m_ScaleBuf = nullptr;
		WGPUBuffer m_RotBuf   = nullptr;
		WGPUBuffer m_ColorBuf = nullptr;

		// Camera uniform: view, proj, viewport. Layout matches the WGSL
		// `Uniforms` struct in `gsplat.wgsl`.
		WGPUBuffer            m_UniformBuf  = nullptr;
		WGPUBindGroupLayout   m_BindLayout  = nullptr;
		WGPUBindGroup         m_BindGroup   = nullptr;
		WGPUPipelineLayout    m_PipeLayout  = nullptr;
		WGPURenderPipeline    m_Pipeline    = nullptr;

		// CPU mirrors for the sort path.
		std::vector<glm::vec3>   m_Positions;
		std::vector<glm::vec3>   m_Scales;
		std::vector<glm::vec4>   m_Rotations;
		std::vector<glm::u8vec4> m_Colors;

		// Sort scratch.
		std::vector<uint32_t>    m_SortIndices;
		std::vector<uint32_t>    m_SortIndicesScratch;
		std::vector<uint32_t>    m_SortKeys;
		std::vector<uint32_t>    m_SortKeysScratch;
		std::vector<glm::vec3>   m_ScratchVec3;
		std::vector<glm::vec4>   m_ScratchVec4;
		std::vector<glm::u8vec4> m_ScratchRgba;
		std::vector<float>       m_Depths;

		// Sort throttling state.
		glm::mat4 m_LastSortView{1.0f};
		bool      m_SortValid = false;
		glm::mat4 m_LastObservedView{1.0f};
		bool      m_WasMovingLastFrame = false;

		size_t m_Count = 0;

		PerfStats              m_LastFrame;
		std::vector<PerfStats> m_History;
		size_t                 m_HistoryHead = 0;
	};

}
