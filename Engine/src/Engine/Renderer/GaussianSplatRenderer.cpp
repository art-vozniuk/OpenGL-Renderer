#include "pch.h"
#include "GaussianSplatRenderer.h"
#include "Assets.h"

#ifndef __EMSCRIPTEN__
#include <glad/glad.h>
#else
#include <GLES3/gl3.h>
#endif

#include <glm/gtc/type_ptr.hpp>

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

		const GLsizeiptr vec3Bytes = static_cast<GLsizeiptr>(m_Count * sizeof(glm::vec3));
		const GLsizeiptr vec4Bytes = static_cast<GLsizeiptr>(m_Count * sizeof(glm::vec4));
		const GLsizeiptr rgbaBytes = static_cast<GLsizeiptr>(m_Count * sizeof(glm::u8vec4));

		glBindBuffer(GL_ARRAY_BUFFER, m_PosVbo);
		glBufferData(GL_ARRAY_BUFFER, vec3Bytes, data.positions.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, m_ScaleVbo);
		glBufferData(GL_ARRAY_BUFFER, vec3Bytes, data.scales.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, m_RotVbo);
		glBufferData(GL_ARRAY_BUFFER, vec4Bytes, data.rotations.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, m_ColorVbo);
		glBufferData(GL_ARRAY_BUFFER, rgbaBytes, data.colors.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		INFO_CORE("GaussianSplatRenderer: uploaded {0} splats to GPU", (uint64_t)m_Count);
	}


	void GaussianSplatRenderer::Render(const SPtr<Camera>& camera, const glm::vec2& viewportSize)
	{
		if (m_Count == 0) return;

		auto shader = AssetManager::GetShader("gsplat");
		shader->Bind();
		shader->UploadUniformMat4("u_View", camera->GetViewMatrix());
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
