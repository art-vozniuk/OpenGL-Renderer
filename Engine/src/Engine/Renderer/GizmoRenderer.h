#pragma once

#include "Camera.h"
#include "WGPUContext.h"
#include "../Core.h"

#include <glm/glm.hpp>
#include <vector>

namespace Engine {

	/*
	 * GizmoRenderer
	 *
	 * Draws thick screen-space line segments — the building block for
	 * every editor gizmo we need. Translate arrows = axis line + V-tip.
	 * Rotate rings = many short chords. Scale handles = axis line +
	 * wireframe cube. Plane handles = small square outlines.
	 *
	 * Usage per frame:
	 *   gizmo.Clear();
	 *   gizmo.AddLine(a, b, color, thicknessPx);
	 *   ... (build all primitives) ...
	 *   gizmo.EncodeRender(pass, camera, viewportSize);
	 *
	 * The renderer rebuilds the line buffer each frame from the queued
	 * primitives — overhead is small (a few hundred lines max). No
	 * depth test: gizmos always draw on top.
	 */
	class GizmoRenderer
	{
	public:
		// One line entry, GPU layout exactly matches `Line` in gizmo.wgsl.
		struct LineEntry
		{
			glm::vec4 a;     // xyz = start, w = thickness in px
			glm::vec4 b;     // xyz = end,   w = unused
			glm::vec4 color; // rgba
		};

		explicit GizmoRenderer(WGPUContext& ctx);
		~GizmoRenderer();

		GizmoRenderer(const GizmoRenderer&)            = delete;
		GizmoRenderer& operator=(const GizmoRenderer&) = delete;

		// Frame-level queue. The renderer copies these into a GPU storage
		// buffer on EncodeRender, growing the buffer if needed.
		void Clear() { m_Lines.clear(); }

		void AddLine(const glm::vec3& a, const glm::vec3& b,
		             const glm::vec4& color, float thicknessPx = 3.0f)
		{
			LineEntry e{};
			e.a = glm::vec4(a, thicknessPx);
			e.b = glm::vec4(b, 0.0f);
			e.color = color;
			m_Lines.push_back(e);
		}

		// High-level primitives. All build on AddLine().
		// `arrow` draws an axis line `pivot → pivot + dir * length` with a
		// 2-line V-tip at the end. dir must be unit length.
		void AddArrow(const glm::vec3& pivot, const glm::vec3& dir, float length,
		              const glm::vec4& color, float thicknessPx = 3.0f);

		// `ring` approximates a circle in the plane perpendicular to `axis`
		// (unit-length, passing through `center`) using `segments` short
		// chords. Radius in world units.
		void AddRing(const glm::vec3& center, const glm::vec3& axis, float radius,
		             const glm::vec4& color, int segments = 48,
		             float thicknessPx = 3.0f);

		// `wireCube` draws a cube of side `size` centered at `center`,
		// axis-aligned. 12 line segments.
		void AddWireCube(const glm::vec3& center, float size,
		                 const glm::vec4& color, float thicknessPx = 2.0f);

		// `planeHandle` draws a small square outline in the XY plane of
		// the given two axes (e.g. X/Y axes for the YZ-locked plane handle
		// at translate-gizmo). Side `size` in world units, offset from
		// `pivot` by half-size along each axis (so the corner sits at
		// pivot and the square spans into the +axisA / +axisB quadrant).
		void AddPlaneHandle(const glm::vec3& pivot,
		                    const glm::vec3& axisA, const glm::vec3& axisB,
		                    float size, const glm::vec4& color,
		                    float thicknessPx = 2.0f);

		// Encode the gizmo draw into an open render pass. Reads the
		// camera's view * proj. Does nothing if no lines queued.
		void EncodeRender(WGPURenderPassEncoder pass,
		                  const SPtr<Camera>& camera,
		                  const glm::vec2& viewportSize);

		size_t LineCount() const { return m_Lines.size(); }

	private:
		void CreatePipeline();
		void EnsureLineBufferCapacity(size_t lineCount);
		void DestroyGpuResources();

		WGPUContext*           m_Ctx = nullptr;

		WGPUBuffer             m_Uniform   = nullptr;
		WGPUBuffer             m_LineBuf   = nullptr;
		size_t                 m_LineCap   = 0;   // capacity in entries
		WGPUBindGroupLayout    m_BGL       = nullptr;
		WGPUBindGroup          m_BG        = nullptr;
		WGPUPipelineLayout     m_PL        = nullptr;
		WGPURenderPipeline     m_Pipe      = nullptr;

		std::vector<LineEntry> m_Lines;
	};

}
