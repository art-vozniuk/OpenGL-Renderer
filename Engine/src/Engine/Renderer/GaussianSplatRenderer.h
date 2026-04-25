#pragma once

#include "SplatLoader.h"
#include "Camera.h"
#include "WGPUContext.h"
#include "../Core.h"

#include <cstdint>
#include <vector>

namespace Engine {

	/*
	 * WebGPU Gaussian-splat renderer with GPU sort.
	 *
	 * Pipeline (per frame):
	 *
	 *   1. cs_init_depth          — view-space depth as sortable u32 + identity perm
	 *   2. for byte b in 0..3:
	 *        cs_clear_hist        — zero histogram[256]
	 *        cs_histogram         — atomicAdd histogram[digit]
	 *        cs_prefix_sum        — exclusive scan -> offsets[256]
	 *        cs_scatter           — atomicAdd offsets[digit] -> idxOut[dst] = idx
	 *      (idxIn / idxOut alternate via the `swap` field of the sort uniform)
	 *   3. render pass            — instance_index -> sortedIndices -> splat data
	 *
	 * Splat data lives in five storage buffers; only `sortedIndices` is
	 * rewritten per frame. Compared to the GL renderer's "reshuffle 4
	 * per-instance VBOs on every sort" design, we save ~80 MB of upload
	 * bandwidth per re-sort at 2 M splats and unlock the per-frame budget
	 * needed to drop the camera-stop-only sort throttle.
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

		// Encode the per-frame sort dispatches into `encoder`. Must run
		// BEFORE OpenColorPass for the same frame (compute and render
		// can't share an encoder once a render pass is open).
		void EncodeSort(WGPUCommandEncoder encoder, const glm::mat4& viewMatrix);

		// Encode the splat draw into `pass`. Must follow EncodeSort in the
		// same frame so `sortedIndices` reflects the current view.
		void EncodeRender(WGPURenderPassEncoder pass,
		                  const SPtr<Camera>& camera,
		                  const glm::vec2& viewportSize);

		size_t SplatCount() const { return m_Count; }

	private:
		void CreateUniformBuffer();
		void CreateSortResources();
		void CreatePipelines();
		void DestroyGpuResources();

		WGPUContext* m_Ctx = nullptr;

		// Storage buffers: read-only splat data (uploaded once).
		WGPUBuffer m_Pos    = nullptr;   // vec4 (xyz padded)
		WGPUBuffer m_Scale  = nullptr;   // vec4 (xyz padded)
		WGPUBuffer m_Rot    = nullptr;   // vec4
		WGPUBuffer m_Color  = nullptr;   // u32 packed (u8x4 normalised)

		// Sort scratch (storage, rewritten every frame).
		WGPUBuffer m_Depths            = nullptr; // array<u32, N>
		WGPUBuffer m_IdxPing           = nullptr; // array<u32, N>
		WGPUBuffer m_IdxPong           = nullptr; // array<u32, N>
		// Per-(workgroup, digit) histogram + same-shape prefix scan output.
		// Layout for both: [digit * numWg + wg]. Sized at upload time.
		WGPUBuffer m_WgHist            = nullptr;
		WGPUBuffer m_WgOffset          = nullptr;
		// Per-digit global bucket start (256 u32).
		WGPUBuffer m_GlobalDigitOffset = nullptr;
		// Per-digit total count, intermediate output of the column scan,
		// input to the digit-offset scan. 256 u32.
		WGPUBuffer m_DigitTotals       = nullptr;

		// Uniforms.
		WGPUBuffer m_RenderUniform = nullptr;  // mat4 view, mat4 proj, vec2 viewport, vec2 pad
		WGPUBuffer m_SortUniform   = nullptr;  // vec4 row2, u32 N, u32 shift, u32 swap, u32 numWg

		// Layouts + groups.
		WGPUBindGroupLayout m_SortBGL    = nullptr;
		WGPUBindGroup       m_SortBG     = nullptr;
		WGPUPipelineLayout  m_SortPL     = nullptr;

		WGPUBindGroupLayout m_RenderBGL  = nullptr;
		WGPUBindGroup       m_RenderBG   = nullptr;
		WGPUPipelineLayout  m_RenderPL   = nullptr;

		// Compute pipelines (one per WGSL entry point).
		WGPUComputePipeline m_PipeInit         = nullptr;
		WGPUComputePipeline m_PipeClearWgHist  = nullptr;
		WGPUComputePipeline m_PipeWgHist       = nullptr;
		WGPUComputePipeline m_PipeColumnScan      = nullptr;
		WGPUComputePipeline m_PipeDigitOffsetScan = nullptr;
		WGPUComputePipeline m_PipeStableScatter = nullptr;

		// Render pipeline.
		WGPURenderPipeline  m_PipeRender     = nullptr;

		// Track which idx buffer is "in" after the last sort pass, so the
		// render bind group can use it as `sortedIndices`. After 4 passes
		// it ends up in IdxPing again (4 swaps), but we keep the explicit
		// state in case the count of passes changes.
		bool m_FinalIsPing = true;

		size_t m_Count = 0;
	};

}
