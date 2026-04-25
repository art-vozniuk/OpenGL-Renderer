#pragma once

#include "SplatLoader.h"
#include "Camera.h"
#include "WGPUContext.h"
#include "../Core.h"
#include "../PerfMetrics.h"

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
		// can't share an encoder once a render pass is open). The
		// projection matrix is needed alongside view to compute the
		// world-space frustum planes for the cull pass.
		void EncodeSort(WGPUCommandEncoder encoder,
		                const glm::mat4& viewMatrix,
		                const glm::mat4& projectionMatrix);

		// Encode the splat draw into `pass`. Must follow EncodeSort in the
		// same frame so `sortedIndices` reflects the current view.
		void EncodeRender(WGPURenderPassEncoder pass,
		                  const SPtr<Camera>& camera,
		                  const glm::vec2& viewportSize);

		size_t SplatCount() const { return m_Count; }

		// ---- Perf instrumentation ------------------------------------------
		// Owned PerfMetrics struct — the scene reads cur/avg/max from this and
		// posts a JSON snapshot to the parent frame each tick. Always
		// populated; `gpuTimingsValid` indicates whether GPU samples are
		// trustworthy (only when the device granted the timestamp-query
		// feature).
		PerfMetrics&       Metrics()       { return m_Metrics; }
		const PerfMetrics& Metrics() const { return m_Metrics; }

		// Pointer the scene must pass to Renderer::OpenColorPass so the
		// render pass writes a begin/end timestamp. Nullptr when timestamp
		// queries aren't available — caller skips the field.
		const WGPUPassTimestampWrites* GetRenderPassTimestampWrites() const
		{
			return m_TimestampsEnabled ? &m_RenderTimestampWrites : nullptr;
		}

		// Resolve the frame's timestamp queries into the next ring slot and
		// kick off async map-back of the slot whose data is ready. Call
		// once per frame, AFTER EncodeRender, while `encoder` is still open.
		void ResolveAndReadTimestamps(WGPUCommandEncoder encoder);

		// Per-frame tick called from the scene at the START of a new frame
		// (i.e. AFTER the previous frame's queue submit). Drives async
		// MapAsync requests on resolved ring slots and pushes any
		// already-mapped samples into m_Metrics. Cheap when there's
		// nothing to do.
		void TickPerf();

		// Inner type exposed publicly so the file-scope mapAsync callback
		// trampoline can hold a pointer to a slot. Not part of the
		// supported renderer API surface — treat as implementation detail.
		struct TsSlot
		{
			WGPUBuffer mapBuf         = nullptr;
			bool       resolved       = false;
			bool       mappingPending = false;
		};

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

		// Indirect-args buffers split in two to satisfy WebGPU's "no
		// writable storage + Indirect on the same buffer in one pass"
		// rule:
		//   m_IndirectArgsStorage — Storage|CopyDst|CopySrc, atomically
		//                          updated by the cull / finalize compute
		//                          kernels via BGL binding 10. Layout:
		//                          [vertexCount, instanceCount,
		//                           firstVertex, firstInstance,
		//                           wgX, wgY, wgZ].
		//   m_IndirectArgsDraw    — Indirect|CopyDst, written each frame
		//                          via CopyBufferToBuffer from the
		//                          storage variant. DrawIndirect at
		//                          offset 0; DispatchIndirect at offset
		//                          16. Never bound as Storage.
		WGPUBuffer m_IndirectArgsStorage = nullptr;
		WGPUBuffer m_IndirectArgsDraw    = nullptr;

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
		WGPUComputePipeline m_PipeClearIndirect   = nullptr;
		WGPUComputePipeline m_PipeInit            = nullptr;
		WGPUComputePipeline m_PipeFinalizeArgs    = nullptr;
		WGPUComputePipeline m_PipeClearWgHist     = nullptr;
		WGPUComputePipeline m_PipeWgHist          = nullptr;
		WGPUComputePipeline m_PipeColumnScan      = nullptr;
		WGPUComputePipeline m_PipeDigitOffsetScan = nullptr;
		WGPUComputePipeline m_PipeStableScatter   = nullptr;

		// Render pipeline.
		WGPURenderPipeline  m_PipeRender     = nullptr;

		// Track which idx buffer is "in" after the last sort pass, so the
		// render bind group can use it as `sortedIndices`. After 4 passes
		// it ends up in IdxPing again (4 swaps), but we keep the explicit
		// state in case the count of passes changes.
		bool m_FinalIsPing = true;

		size_t m_Count = 0;

		// ---- Perf instrumentation ------------------------------------------
		PerfMetrics m_Metrics;

		// Timestamp infrastructure. 4 slots: sort-begin (0), sort-end (1),
		// render-begin (2), render-end (3). Render-side begin/end is set
		// via the render-pass descriptor (RenderPassTimestampWrites);
		// sort-side via compute-pass descriptors injected in EncodeSort.
		bool                                 m_TimestampsEnabled = false;
		WGPUQuerySet                         m_QuerySet         = nullptr;
		WGPUBuffer                           m_TsResolveBuf     = nullptr;
		WGPUPassTimestampWrites       m_SortBeginTimestampWrites{};
		WGPUPassTimestampWrites       m_SortEndTimestampWrites{};
		WGPUPassTimestampWrites        m_RenderTimestampWrites{};

		// 3-deep ring of MAP_READ-mappable buffers so the GPU can be writing
		// frame N while CPU reads frame N-2. The TsSlot struct itself is
		// declared above (public for callback access).
		static constexpr int kTsRingSize = 3;
		TsSlot                          m_TsRing[kTsRingSize];
		int                             m_TsRingNext = 0;
		double                          m_TsPeriodNs = 1.0;  // device timestamp tick → ns; Dawn currently always ns

		// CPU-side encode timer.
		double m_LastFrameStart = 0.0;

		// Helpers
		void CreateTimestampResources();
		void DestroyTimestampResources();
	};

}
