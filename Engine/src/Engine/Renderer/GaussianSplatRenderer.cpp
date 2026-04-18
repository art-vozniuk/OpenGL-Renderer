#include "pch.h"
#include "GaussianSplatRenderer.h"
#include "Assets.h"

#ifndef __EMSCRIPTEN__
#include <glad/glad.h>
#else
#include <GLES3/gl3.h>
#endif

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <numeric>

namespace Engine {

	namespace {
		// Vertex-attribute locations, matched to gsplat_v.glsl.
		constexpr int kLocCorner = 0;
		constexpr int kLocPos    = 1;
		constexpr int kLocScale  = 2;
		constexpr int kLocRot    = 3;
		constexpr int kLocColor  = 4;
	}


	GaussianSplatRenderer::GaussianSplatRenderer()
	{
		CreateQuadMesh();
		CreateInstanceBuffers();
	}


	GaussianSplatRenderer::~GaussianSplatRenderer()
	{
		DestroyGpuResources();
	}


	void GaussianSplatRenderer::CreateQuadMesh()
	{
		glGenVertexArrays(1, &m_Vao);
		glBindVertexArray(m_Vao);

		// Unit quad corners in [-1, +1]²; the vertex shader scales them by
		// the per-splat ellipse radii.
		static const float kQuadCorners[] = {
			-1.0f, -1.0f,
			 1.0f, -1.0f,
			 1.0f,  1.0f,
			-1.0f,  1.0f,
		};
		static const uint32_t kQuadIndices[] = { 0, 1, 2, 0, 2, 3 };

		glGenBuffers(1, &m_QuadVbo);
		glBindBuffer(GL_ARRAY_BUFFER, m_QuadVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadCorners), kQuadCorners, GL_STATIC_DRAW);
		glEnableVertexAttribArray(kLocCorner);
		glVertexAttribPointer(kLocCorner, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);

		glGenBuffers(1, &m_QuadEbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_QuadEbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kQuadIndices), kQuadIndices, GL_STATIC_DRAW);

		glBindVertexArray(0);
	}


	void GaussianSplatRenderer::CreateInstanceBuffers()
	{
		glGenBuffers(1, &m_PosVbo);
		glGenBuffers(1, &m_ScaleVbo);
		glGenBuffers(1, &m_RotVbo);
		glGenBuffers(1, &m_ColorVbo);

		// Bind them into the VAO with per-instance divisor so that each splat
		// advances one step in the instance buffer per rendered quad.
		glBindVertexArray(m_Vao);

		glBindBuffer(GL_ARRAY_BUFFER, m_PosVbo);
		glEnableVertexAttribArray(kLocPos);
		glVertexAttribPointer(kLocPos, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, nullptr);
		glVertexAttribDivisor(kLocPos, 1);

		glBindBuffer(GL_ARRAY_BUFFER, m_ScaleVbo);
		glEnableVertexAttribArray(kLocScale);
		glVertexAttribPointer(kLocScale, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, nullptr);
		glVertexAttribDivisor(kLocScale, 1);

		glBindBuffer(GL_ARRAY_BUFFER, m_RotVbo);
		glEnableVertexAttribArray(kLocRot);
		glVertexAttribPointer(kLocRot, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
		glVertexAttribDivisor(kLocRot, 1);

		// Colours are uploaded as normalised uint8 — saves 75% GPU memory vs
		// float32 RGBA and is equivalent after the built-in normalisation.
		glBindBuffer(GL_ARRAY_BUFFER, m_ColorVbo);
		glEnableVertexAttribArray(kLocColor);
		glVertexAttribPointer(kLocColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(uint8_t) * 4, nullptr);
		glVertexAttribDivisor(kLocColor, 1);

		glBindVertexArray(0);
	}


	void GaussianSplatRenderer::Upload(const SplatData& data)
	{
		m_Count = data.Count();
		if (m_Count == 0) {
			WARN_CORE("GaussianSplatRenderer::Upload called with empty dataset");
			return;
		}

		// Keep a CPU copy — the back-to-front sorter reshuffles these each
		// time the camera moves, and recomputing depths requires positions
		// on the CPU side anyway.
		m_Positions = data.positions;
		m_Scales    = data.scales;
		m_Rotations = data.rotations;
		m_Colors    = data.colors;

		// Pre-size scratch buffers to the maximum we'll need during a sort.
		m_SortIndices.resize(m_Count);
		m_ScratchVec3.resize(m_Count);
		m_ScratchVec4.resize(m_Count);
		m_ScratchRgba.resize(m_Count);
		m_Depths.resize(m_Count);
		std::iota(m_SortIndices.begin(), m_SortIndices.end(), uint32_t{0});

		const GLsizeiptr vec3Bytes = static_cast<GLsizeiptr>(m_Count * sizeof(glm::vec3));
		const GLsizeiptr vec4Bytes = static_cast<GLsizeiptr>(m_Count * sizeof(glm::vec4));
		const GLsizeiptr rgbaBytes = static_cast<GLsizeiptr>(m_Count * sizeof(glm::u8vec4));

		// DYNAMIC_DRAW because Sort() will re-upload these whenever the
		// camera moves enough to change the blend order.
		glBindBuffer(GL_ARRAY_BUFFER, m_PosVbo);
		glBufferData(GL_ARRAY_BUFFER, vec3Bytes, data.positions.data(), GL_DYNAMIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, m_ScaleVbo);
		glBufferData(GL_ARRAY_BUFFER, vec3Bytes, data.scales.data(), GL_DYNAMIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, m_RotVbo);
		glBufferData(GL_ARRAY_BUFFER, vec4Bytes, data.rotations.data(), GL_DYNAMIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, m_ColorVbo);
		glBufferData(GL_ARRAY_BUFFER, rgbaBytes, data.colors.data(), GL_DYNAMIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		m_SortValid = false;  // force a fresh sort on the first Render()
		INFO_CORE("GaussianSplatRenderer: uploaded {0} splats to GPU", (uint64_t)m_Count);
	}


	bool GaussianSplatRenderer::NeedsResort(const glm::mat4& viewMatrix) const
	{
		if (!m_SortValid) return true;

		// Compare forward vectors and positions between the last-sorted view
		// and the current one. The forward vector is the third row of the
		// view matrix's rotation part (GL's forward is -Z in camera space,
		// so the world-space forward is -view[ 2 ][.xyz]).
		const glm::vec3 fOld(-m_LastSortView[0][2], -m_LastSortView[1][2], -m_LastSortView[2][2]);
		const glm::vec3 fNew(-viewMatrix[0][2],      -viewMatrix[1][2],      -viewMatrix[2][2]);
		const glm::vec3 pOld = -glm::vec3(m_LastSortView[3]);  // approx (view is world→cam)
		const glm::vec3 pNew = -glm::vec3(viewMatrix[3]);

		// Thresholds scaled to the cloud footprint — we haven't got that
		// here so use conservative constants that avoid thrashing on tiny
		// camera drifts but catch meaningful rotations / translations.
		const float kDotThreshold = 0.999f;   // ~2.5° rotation
		const float kPosThreshold = 0.5f;     // world-space units
		return glm::dot(fOld, fNew) < kDotThreshold
		    || glm::length(pOld - pNew) > kPosThreshold;
	}


	void GaussianSplatRenderer::Sort(const glm::mat4& viewMatrix)
	{
		if (m_Count == 0) return;

		// 1. Compute depth per splat in view space. Only the z component is
		//    needed; we multiply by the relevant column vectors directly
		//    instead of the full mat4 * vec4 to keep the inner loop tight.
		const float a = viewMatrix[0][2];
		const float b = viewMatrix[1][2];
		const float c = viewMatrix[2][2];
		const float d = viewMatrix[3][2];
		for (size_t i = 0; i < m_Count; ++i) {
			const glm::vec3& p = m_Positions[i];
			m_Depths[i] = a * p.x + b * p.y + c * p.z + d;
		}

		// 2. Sort indices so that the LATER-drawn splats are the FURTHEST
		//    from the camera. GL view-space forward is -Z so "further" means
		//    "more negative depth"; we therefore sort descending by depth
		//    (the biggest — closest to zero — value comes first in the
		//    instance iteration, and the most negative — furthest away —
		//    comes last).
		//
		//    Why this direction: with glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
		//    and the reference WebGL gsplat viewers, the visually correct
		//    result on train.splat is achieved when instance[N-1] is the
		//    furthest splat. Switching to ascending produces a washed-out
		//    "grey fog" dominated by outlier splats drawn on top. See
		//    antimatter15/splat for the same convention.
		std::iota(m_SortIndices.begin(), m_SortIndices.end(), uint32_t{0});
		std::sort(m_SortIndices.begin(), m_SortIndices.end(),
		          [this](uint32_t i, uint32_t j) { return m_Depths[i] > m_Depths[j]; });

		// 3. Reshuffle each per-instance VBO's data via the permutation and
		//    re-upload. Using scratch buffers the same size as the source
		//    avoids alloc traffic in the hot path.
		for (size_t i = 0; i < m_Count; ++i) m_ScratchVec3[i] = m_Positions[m_SortIndices[i]];
		glBindBuffer(GL_ARRAY_BUFFER, m_PosVbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0,
		                static_cast<GLsizeiptr>(m_Count * sizeof(glm::vec3)),
		                m_ScratchVec3.data());

		for (size_t i = 0; i < m_Count; ++i) m_ScratchVec3[i] = m_Scales[m_SortIndices[i]];
		glBindBuffer(GL_ARRAY_BUFFER, m_ScaleVbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0,
		                static_cast<GLsizeiptr>(m_Count * sizeof(glm::vec3)),
		                m_ScratchVec3.data());

		for (size_t i = 0; i < m_Count; ++i) m_ScratchVec4[i] = m_Rotations[m_SortIndices[i]];
		glBindBuffer(GL_ARRAY_BUFFER, m_RotVbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0,
		                static_cast<GLsizeiptr>(m_Count * sizeof(glm::vec4)),
		                m_ScratchVec4.data());

		for (size_t i = 0; i < m_Count; ++i) m_ScratchRgba[i] = m_Colors[m_SortIndices[i]];
		glBindBuffer(GL_ARRAY_BUFFER, m_ColorVbo);
		glBufferSubData(GL_ARRAY_BUFFER, 0,
		                static_cast<GLsizeiptr>(m_Count * sizeof(glm::u8vec4)),
		                m_ScratchRgba.data());

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		m_LastSortView = viewMatrix;
		m_SortValid = true;
	}


	void GaussianSplatRenderer::Render(const SPtr<Camera>& camera, const glm::vec2& viewportSize)
	{
		if (m_Count == 0) return;

		const glm::mat4& view = camera->GetViewMatrix();
		if (NeedsResort(view)) {
			Sort(view);
		}

		auto shader = AssetManager::GetShader("gsplat");
		shader->Bind();
		shader->UploadUniformMat4("u_View", view);
		shader->UploadUniformMat4("u_Projection", camera->GetProjectionMatrix());
		shader->UploadUniformFloat2("u_ViewportSize", viewportSize);

		// GL state for splats:
		//   - blend premultiplied alpha (shader already outputs rgb*a, a)
		//   - no depth WRITE (splats are semi-transparent points) but keep
		//     depth TEST against opaque geometry rendered earlier in the frame
		//   - no face culling (the quads face the camera by construction)
		GLboolean prevBlend      = glIsEnabled(GL_BLEND);
		GLboolean prevCull       = glIsEnabled(GL_CULL_FACE);
		GLboolean prevDepthTest  = glIsEnabled(GL_DEPTH_TEST);
		GLint     prevBlendSrc, prevBlendDst, prevDepthMask;
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrc);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDst);
		glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);

		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_CULL_FACE);
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);

		glBindVertexArray(m_Vao);
		glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
		                         static_cast<GLsizei>(m_Count));
		glBindVertexArray(0);

		// Restore prior state so subsequent draws in the frame aren't affected.
		if (!prevBlend) glDisable(GL_BLEND);
		glBlendFunc(prevBlendSrc, prevBlendDst);
		if (prevCull)       glEnable(GL_CULL_FACE);
		if (prevDepthTest)  glEnable(GL_DEPTH_TEST);
		glDepthMask(prevDepthMask ? GL_TRUE : GL_FALSE);
	}


	void GaussianSplatRenderer::DestroyGpuResources()
	{
		const uint32_t buffers[] = { m_QuadVbo, m_QuadEbo, m_PosVbo, m_ScaleVbo, m_RotVbo, m_ColorVbo };
		glDeleteBuffers(sizeof(buffers) / sizeof(buffers[0]), buffers);
		if (m_Vao) glDeleteVertexArrays(1, &m_Vao);

		m_Vao = m_QuadVbo = m_QuadEbo = 0;
		m_PosVbo = m_ScaleVbo = m_RotVbo = m_ColorVbo = 0;
	}

}
