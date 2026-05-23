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

		// World-space filled triangle. Layout matches `Tri` in gizmo_tri.wgsl.
		struct TriEntry
		{
			glm::vec4 a;
			glm::vec4 b;
			glm::vec4 c;
			glm::vec4 color;
		};

		explicit GizmoRenderer(WGPUContext& ctx);
		~GizmoRenderer();

		GizmoRenderer(const GizmoRenderer&)            = delete;
		GizmoRenderer& operator=(const GizmoRenderer&) = delete;

		// Frame-level queue. The renderer copies these into a GPU storage
		// buffer on EncodeRender, growing the buffer if needed.
		void Clear() { m_Lines.clear(); m_Tris.clear(); }

		void AddLine(const glm::vec3& a, const glm::vec3& b,
		             const glm::vec4& color, float thicknessPx = 3.0f)
		{
			LineEntry e{};
			e.a = glm::vec4(a, thicknessPx);
			e.b = glm::vec4(b, 0.0f);
			e.color = color;
			m_Lines.push_back(e);
		}

		void AddTri(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
		            const glm::vec4& color)
		{
			TriEntry e{};
			e.a = glm::vec4(a, 0.0f);
			e.b = glm::vec4(b, 0.0f);
			e.c = glm::vec4(c, 0.0f);
			e.color = color;
			m_Tris.push_back(e);
		}

		// Camera-facing billboard arrowhead. Apex at `tip`; base perpendicular
		// to `axisDir` at length-`headLen` back from the tip, width `headWidth`.
		// `cameraPos` is used to orient the billboard's "sideways".
		void AddArrowHead(const glm::vec3& tip, const glm::vec3& axisDir,
		                  float headLen, float headWidth,
		                  const glm::vec3& cameraPos, const glm::vec4& color);

		// Filled hexagonal disk facing `cameraPos`, world-space radius `r`.
		void AddDisk(const glm::vec3& center, float radius,
		             const glm::vec3& cameraPos, const glm::vec4& color,
		             int segments = 12);

		// Partial ring — `startRad..endRad` around `center` in plane perp to
		// `axis`. Use this for rotate handles drawn as quarter / half arcs
		// instead of full rings.
		void AddArc(const glm::vec3& center, const glm::vec3& axis, float radius,
		            float startRad, float endRad,
		            const glm::vec4& color, int segments = 32,
		            float thicknessPx = 3.0f);

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
		size_t TriCount()  const { return m_Tris.size();  }

	private:
		void CreateLinePipeline();
		void CreateTriPipeline();
		void EnsureLineBufferCapacity(size_t lineCount);
		void EnsureTriBufferCapacity(size_t triCount);
		void DestroyGpuResources();

		WGPUContext*           m_Ctx = nullptr;
		WGPUBuffer             m_Uniform   = nullptr;

		WGPUBuffer             m_LineBuf   = nullptr;
		size_t                 m_LineCap   = 0;
		WGPUBindGroupLayout    m_LineBGL   = nullptr;
		WGPUBindGroup          m_LineBG    = nullptr;
		WGPUPipelineLayout     m_LinePL    = nullptr;
		WGPURenderPipeline     m_LinePipe  = nullptr;

		WGPUBuffer             m_TriBuf    = nullptr;
		size_t                 m_TriCap    = 0;
		WGPUBindGroupLayout    m_TriBGL    = nullptr;
		WGPUBindGroup          m_TriBG     = nullptr;
		WGPUPipelineLayout     m_TriPL     = nullptr;
		WGPURenderPipeline     m_TriPipe   = nullptr;

		std::vector<LineEntry> m_Lines;
		std::vector<TriEntry>  m_Tris;
	};

}
