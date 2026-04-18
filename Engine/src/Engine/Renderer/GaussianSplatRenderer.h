#pragma once

#include "SplatLoader.h"
#include "Shader.h"
#include "Camera.h"
#include "../Core.h"

#include <cstdint>
#include <vector>

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

		// Runs an immediate back-to-front sort outside the render loop.
		// Call once after Upload() + initial camera setup so the first
		// visible frame is already sorted (otherwise the first frame
		// renders in file order and only gets sorted on frame 2).
		void SortNow(const glm::mat4& viewMatrix) { Sort(viewMatrix); }

		// Per-stage timing snapshot — the GUI reads this each frame to show
		// a "max over the last N seconds" number for each phase. A stage
		// with a zero sample this frame (e.g. no sort ran) is ignored when
		// computing the rolling max.
		struct PerfStats {
			float sortMs      = 0.0f;  // std::sort + index build
			float reshuffleMs = 0.0f;  // CPU scratch-buffer reorder
			float uploadMs    = 0.0f;  // 4x glBufferSubData
			float drawMs      = 0.0f;  // glDrawElementsInstanced + glFinish
		};

		PerfStats LastFrame() const { return m_LastFrame; }

		// Rolling max over ~5 s of the per-stage measurements above.
		PerfStats MaxLast5s() const;

	private:
		void CreateQuadMesh();
		void CreateInstanceBuffers();
		void DestroyGpuResources();

		// Sort splats back-to-front relative to the camera and upload the
		// reordered per-instance data to the GPU. Expensive (~50 ms for 1 M
		// splats); caller is responsible for throttling.
		void Sort(const glm::mat4& viewMatrix);

		// Returns true if the camera has moved enough since the last sort
		// that the visual ordering would drift. Used to trigger debounced
		// re-sorts without wasting cycles on a static view.
		bool NeedsResort(const glm::mat4& viewMatrix) const;

		// GL handles. Held as uint32_t to avoid leaking <glad>/<GLES3> from
		// this header into translation units that don't need them.
		uint32_t m_Vao = 0;
		uint32_t m_QuadVbo = 0;
		uint32_t m_QuadEbo = 0;

		uint32_t m_PosVbo   = 0;
		uint32_t m_ScaleVbo = 0;
		uint32_t m_RotVbo   = 0;
		uint32_t m_ColorVbo = 0;

		// CPU-side copies of the original per-splat data, kept so the sorter
		// can reorder rows without round-tripping through the GPU. These are
		// also the source of truth for recomputing depths each sort pass.
		std::vector<glm::vec3>   m_Positions;
		std::vector<glm::vec3>   m_Scales;
		std::vector<glm::vec4>   m_Rotations;
		std::vector<glm::u8vec4> m_Colors;

		// Scratch storage for the permutation + per-sort reshuffle. Kept as
		// members so we don't pay for alloc/free every sort.
		std::vector<uint32_t>    m_SortIndices;
		std::vector<uint32_t>    m_SortIndicesScratch;  // radix-sort ping-pong buffer
		std::vector<uint32_t>    m_SortKeys;            // depths cast to sortable uint32
		std::vector<uint32_t>    m_SortKeysScratch;
		std::vector<glm::vec3>   m_ScratchVec3;
		std::vector<glm::vec4>   m_ScratchVec4;
		std::vector<glm::u8vec4> m_ScratchRgba;
		std::vector<float>       m_Depths;

		// Last view matrix used for sorting — NeedsResort() compares against
		// the current view to decide whether the existing order is still
		// good enough.
		glm::mat4 m_LastSortView{1.0f};
		bool      m_SortValid = false;

		// Motion detection: sort runs only on the frame after the camera
		// comes to rest. We keep the previous-frame view to diff against
		// the current view, plus a boolean recording whether motion was
		// observed last frame.
		glm::mat4 m_LastObservedView{1.0f};
		bool      m_WasMovingLastFrame = false;

		size_t m_Count = 0;

		// Timing buffers. We keep up to ~5 s of history at 60 fps ≈ 300 frames.
		PerfStats              m_LastFrame;
		std::vector<PerfStats> m_History;
		size_t                 m_HistoryHead = 0;
	};

}
