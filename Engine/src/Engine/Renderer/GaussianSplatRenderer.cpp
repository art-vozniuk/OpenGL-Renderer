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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <numeric>

namespace Engine {

	namespace {
		// Vertex-attribute locations, matched to gsplat_v.glsl.
		constexpr int kLocCorner  = 0;
		constexpr int kLocPos     = 1;
		constexpr int kLocScale   = 2;
		constexpr int kLocRot     = 3;
		constexpr int kLocColor   = 4;
		constexpr int kLocOrigIdx = 5;  // SH variant only

		// 5-second window at 60 fps — the GUI shows "max in last ~5 s" so
		// we keep that many frame-samples on hand.
		constexpr size_t kHistoryFrames = 300;

		using Clock = std::chrono::steady_clock;
		inline float ElapsedMs(Clock::time_point t0, Clock::time_point t1)
		{
			return std::chrono::duration<float, std::milli>(t1 - t0).count();
		}
	}


	GaussianSplatRenderer::GaussianSplatRenderer()
	{
		CreateQuadMesh();
		CreateInstanceBuffers();
		m_History.assign(kHistoryFrames, PerfStats{});

		// Initial state of the SH-disabled toggle can be seeded from the env
		// so headless capture runs (HQ_SH_OFF=1 …) still work. The UI can
		// still flip it at runtime.
		const char* envOff = std::getenv("HQ_SH_OFF");
		m_ShDisabled = (envOff && *envOff && envOff[0] != '0');
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
		glGenBuffers(1, &m_OrigIdxVbo);

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

		// Original splat index — integer attribute. Must use glVertexAttribIPointer
		// (not Pointer) so GLSL `in int` receives the raw value instead of a
		// float cast. Divisor 1 → one per instance.
		glBindBuffer(GL_ARRAY_BUFFER, m_OrigIdxVbo);
		glEnableVertexAttribArray(kLocOrigIdx);
		glVertexAttribIPointer(kLocOrigIdx, 1, GL_UNSIGNED_INT, sizeof(uint32_t), nullptr);
		glVertexAttribDivisor(kLocOrigIdx, 1);

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
		m_SortIndicesScratch.resize(m_Count);
		m_SortKeys.resize(m_Count);
		m_SortKeysScratch.resize(m_Count);
		m_ScratchVec3.resize(m_Count);
		m_ScratchVec4.resize(m_Count);
		m_ScratchRgba.resize(m_Count);
		m_ScratchU32.resize(m_Count);
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

		// Initial identity permutation for the orig-index VBO. Sort will
		// reshuffle this buffer alongside pos/scale/rot/color each time it
		// fires, so the shader always sees "shader instance i → SH texture
		// row X" mapping in sync with the attribute data.
		{
			std::vector<uint32_t> identity(m_Count);
			std::iota(identity.begin(), identity.end(), uint32_t{0});
			glBindBuffer(GL_ARRAY_BUFFER, m_OrigIdxVbo);
			glBufferData(GL_ARRAY_BUFFER,
			             static_cast<GLsizeiptr>(m_Count * sizeof(uint32_t)),
			             identity.data(), GL_DYNAMIC_DRAW);
		}

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		// Optional SH (bands 1..3) upload as a 2D RGBA32F texture, tiled to
		// stay within GL_MAX_TEXTURE_SIZE on both axes. Present only for
		// Inria .ply datasets; antimatter15 .splat never has it. We tear
		// down any previous texture first so Upload() is idempotent on reload.
		if (m_ShTex) {
			glDeleteTextures(1, &m_ShTex);
			m_ShTex = 0;
		}
		m_ShCoefCount    = 0;
		m_ShSplatsPerRow = 0;
		if (data.HasSH()) {
			m_ShCoefCount = data.shCoefCount;

			// Pack K splats side-by-side per row so the texture is neither
			// too tall (Apple caps GL_MAX_TEXTURE_SIZE at 16384, our splat
			// count can be 1M+) nor forced into 3D. Pick K as the largest
			// power of two that keeps width ≤ maxTex, then derive height.
			GLint maxTex = 0;
			glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
			if (maxTex <= 0) maxTex = 16384;  // defensive default
			int splatsPerRow = 1;
			while ((splatsPerRow * 2) * m_ShCoefCount <= maxTex) {
				splatsPerRow *= 2;
			}
			m_ShSplatsPerRow = splatsPerRow;
			const int texW = splatsPerRow * m_ShCoefCount;
			const int texH = static_cast<int>((m_Count + splatsPerRow - 1) / splatsPerRow);

			// Build a padded RGBA32F buffer on the CPU. The alpha channel is
			// zeroed; the shader only samples .rgb.
			std::vector<float> texBuffer(static_cast<size_t>(texW) * texH * 4, 0.0f);
			for (size_t i = 0; i < m_Count; ++i) {
				const size_t row = i / splatsPerRow;
				const size_t col = (i % splatsPerRow) * m_ShCoefCount;
				const float* src = data.shRest.data() + i * m_ShCoefCount * 3;
				for (int c = 0; c < m_ShCoefCount; ++c) {
					float* dst = texBuffer.data() + ((row * texW) + col + c) * 4;
					dst[0] = src[c * 3 + 0];
					dst[1] = src[c * 3 + 1];
					dst[2] = src[c * 3 + 2];
					// dst[3] = 0 (padding)
				}
			}

			glGenTextures(1, &m_ShTex);
			glBindTexture(GL_TEXTURE_2D, m_ShTex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F,
			             texW, texH,
			             0, GL_RGBA, GL_FLOAT, texBuffer.data());
			glBindTexture(GL_TEXTURE_2D, 0);
			INFO_CORE("GaussianSplatRenderer: SH texture {0}x{1} RGBA32F ({2} MB), {3} splats/row",
			          texW, texH,
			          (uint64_t)((size_t)texW * texH * 4 * sizeof(float) / (1024 * 1024)),
			          splatsPerRow);
		}

		m_SortValid = false;  // force a fresh sort on the first Render()
		INFO_CORE("GaussianSplatRenderer: uploaded {0} splats to GPU", (uint64_t)m_Count);
	}


	bool GaussianSplatRenderer::NeedsResort(const glm::mat4& viewMatrix) const
	{
		// First frame: no sort yet.
		if (!m_SortValid) return true;

		// Motion is the only trigger we care about: compare against the view
		// observed last frame. Moving continuously → skip sort (avoids per-
		// frame stutter). Only when the camera comes to rest (was moving,
		// now still) do we reorder for clean blending.
		const glm::vec3 fPrev(-m_LastObservedView[0][2], -m_LastObservedView[1][2], -m_LastObservedView[2][2]);
		const glm::vec3 fNow (-viewMatrix[0][2],         -viewMatrix[1][2],         -viewMatrix[2][2]);
		const glm::vec3 pPrev = -glm::vec3(m_LastObservedView[3]);
		const glm::vec3 pNow  = -glm::vec3(viewMatrix[3]);
		const bool movingNow = glm::dot(fPrev, fNow) < 0.99999f
		                    || glm::length(pPrev - pNow) > 1e-4f;

		return m_WasMovingLastFrame && !movingNow;
	}


	void GaussianSplatRenderer::Sort(const glm::mat4& viewMatrix)
	{
		if (m_Count == 0) return;

		auto tStart = Clock::now();

		// 1. Per-splat view-space depth (only z component needed). We also
		//    bit-cast each float into a sortable uint32 on the fly so the
		//    radix pass below doesn't need to dereference m_Depths.
		//
		//    Sortable encoding: flip sign bit for positives, all bits for
		//    negatives. Result: ascending uint32 compare == ascending float.
		const float a = viewMatrix[0][2];
		const float b = viewMatrix[1][2];
		const float c = viewMatrix[2][2];
		const float d = viewMatrix[3][2];
		for (size_t i = 0; i < m_Count; ++i) {
			const glm::vec3& p = m_Positions[i];
			float f = a * p.x + b * p.y + c * p.z + d;
			m_Depths[i] = f;
			uint32_t u;
			std::memcpy(&u, &f, 4);
			m_SortKeys[i] = (u & 0x80000000u) ? ~u : (u ^ 0x80000000u);
		}

		// 2. LSD radix sort by 8-bit digits, 4 passes. Each pass does a
		//    counting-sort on one byte of the 32-bit key. std::sort on 1 M
		//    floats measured at ~200 ms; radix hits ~20-30 ms on M2.
		std::iota(m_SortIndices.begin(), m_SortIndices.end(), uint32_t{0});
		for (int byteIdx = 0; byteIdx < 4; ++byteIdx) {
			const int shift = byteIdx * 8;
			uint32_t buckets[256] = {0};
			for (size_t i = 0; i < m_Count; ++i)
				++buckets[(m_SortKeys[i] >> shift) & 0xFFu];
			uint32_t sum = 0;
			for (int b = 0; b < 256; ++b) {
				uint32_t c2 = buckets[b];
				buckets[b] = sum;
				sum += c2;
			}
			for (size_t i = 0; i < m_Count; ++i) {
				uint32_t k  = m_SortKeys[i];
				uint32_t id = m_SortIndices[i];
				uint32_t dst = buckets[(k >> shift) & 0xFFu]++;
				m_SortKeysScratch[dst]    = k;
				m_SortIndicesScratch[dst] = id;
			}
			m_SortKeys.swap(m_SortKeysScratch);
			m_SortIndices.swap(m_SortIndicesScratch);
		}

		auto tSorted = Clock::now();

		// 3a. CPU-side reshuffle of each attribute into scratch storage.
		for (size_t i = 0; i < m_Count; ++i) m_ScratchVec3[i] = m_Positions[m_SortIndices[i]];
		// pos done — fall through; interleave uploads with reshuffles to
		// minimize peak scratch working set (each scratch buffer is reused).

		auto tPosReshuffle = Clock::now();

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

		// Carry the original splat index alongside the reshuffle — the SH
		// shader reads this as a per-instance attribute to look up the
		// correct row in the (unsorted) SH texture.
		if (m_ShTex) {
			for (size_t i = 0; i < m_Count; ++i) m_ScratchU32[i] = m_SortIndices[i];
			glBindBuffer(GL_ARRAY_BUFFER, m_OrigIdxVbo);
			glBufferSubData(GL_ARRAY_BUFFER, 0,
			                static_cast<GLsizeiptr>(m_Count * sizeof(uint32_t)),
			                m_ScratchU32.data());
		}

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		auto tUploaded = Clock::now();

		// The sort/reshuffle/upload split is approximate: we only time the
		// FIRST reshuffle separately; subsequent reshuffles happen inline
		// with their upload and count toward "upload". Good enough to tell
		// "am I CPU-bound on std::sort" vs "am I bandwidth-bound on GPU upload".
		m_LastFrame.sortMs      = ElapsedMs(tStart, tSorted);
		m_LastFrame.reshuffleMs = ElapsedMs(tSorted, tPosReshuffle);
		m_LastFrame.uploadMs    = ElapsedMs(tPosReshuffle, tUploaded);

		m_LastSortView = viewMatrix;
		m_SortValid = true;
	}


	void GaussianSplatRenderer::Render(const SPtr<Camera>& camera, const glm::vec2& viewportSize)
	{
		if (m_Count == 0) return;

		// Reset per-frame stats. Sort() fills in sort/reshuffle/upload when
		// it runs (stages left at 0 mean "didn't happen this frame" and get
		// ignored when computing the rolling max).
		m_LastFrame = PerfStats{};

		const glm::mat4& view = camera->GetViewMatrix();

		// Compute "moving this frame" from the view delta before NeedsResort
		// runs, so the throttle can tell moving-stopped from still-moving.
		const glm::vec3 fPrev(-m_LastObservedView[0][2], -m_LastObservedView[1][2], -m_LastObservedView[2][2]);
		const glm::vec3 fNow (-view[0][2],               -view[1][2],               -view[2][2]);
		const glm::vec3 pPrev = -glm::vec3(m_LastObservedView[3]);
		const glm::vec3 pNow  = -glm::vec3(view[3]);
		const bool movingNow = glm::dot(fPrev, fNow) < 0.99999f
		                    || glm::length(pPrev - pNow) > 1e-4f;

		if (NeedsResort(view)) {
			Sort(view);
		}

		m_LastObservedView   = view;
		m_WasMovingLastFrame = movingNow;

		// Two shader variants share the same draw setup. The SH variant reads
		// an extra texture + camera position to evaluate bands 1..3 per splat.
		// `m_ShDisabled` (ImGui toggle / HQ_SH_OFF env var) forces the flat
		// colour path even when SH data is loaded — lets the UI compare
		// "with SH" vs "without SH" on the same dataset.
		const bool useSh = (m_ShTex != 0) && !m_ShDisabled;
		auto shader = AssetManager::GetShader(useSh ? "gsplat_sh" : "gsplat");
		shader->Bind();
		shader->UploadUniformMat4("u_View", view);
		shader->UploadUniformMat4("u_Projection", camera->GetProjectionMatrix());
		shader->UploadUniformFloat2("u_ViewportSize", viewportSize);

		if (useSh) {
			// Derive world-space camera position from the view matrix:
			//   view = [R | -R * cam]   ⇒   cam = -Rᵀ * t.
			// Cheaper than glm::inverse(view) and numerically identical for
			// orthonormal R (which our camera always produces).
			const glm::mat3 R(view);
			const glm::vec3 t(view[3]);
			const glm::vec3 camPos = -glm::transpose(R) * t;
			shader->UploadUniformFloat3("u_CameraPos", camPos);
			shader->UploadUniformInt("u_ShTex", 0);
			shader->UploadUniformInt("u_ShCoefCount", m_ShCoefCount);
			shader->UploadUniformInt("u_ShSplatsPerRow", m_ShSplatsPerRow);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_ShTex);
		}

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

		auto tDrawStart = Clock::now();
		glBindVertexArray(m_Vao);
		glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
		                         static_cast<GLsizei>(m_Count));
		glBindVertexArray(0);
		// glFinish forces the GPU to complete the draw before returning, so
		// the measurement reflects real GPU work instead of only submission
		// latency. This slows the frame, but it's the only way to get a
		// meaningful per-stage number without GL timer queries (WebGL 2
		// doesn't expose them).
		glFinish();
		m_LastFrame.drawMs = ElapsedMs(tDrawStart, Clock::now());

		// Ring-buffer the completed frame's stats for the GUI to inspect.
		m_History[m_HistoryHead] = m_LastFrame;
		m_HistoryHead = (m_HistoryHead + 1) % m_History.size();

		// Restore prior state so subsequent draws in the frame aren't affected.
		if (!prevBlend) glDisable(GL_BLEND);
		glBlendFunc(prevBlendSrc, prevBlendDst);
		if (prevCull)       glEnable(GL_CULL_FACE);
		if (prevDepthTest)  glEnable(GL_DEPTH_TEST);
		glDepthMask(prevDepthMask ? GL_TRUE : GL_FALSE);
	}


	GaussianSplatRenderer::PerfStats GaussianSplatRenderer::MaxLast5s() const
	{
		PerfStats m{};
		for (const auto& s : m_History) {
			m.sortMs      = std::max(m.sortMs,      s.sortMs);
			m.reshuffleMs = std::max(m.reshuffleMs, s.reshuffleMs);
			m.uploadMs    = std::max(m.uploadMs,    s.uploadMs);
			m.drawMs      = std::max(m.drawMs,      s.drawMs);
		}
		return m;
	}


	void GaussianSplatRenderer::DestroyGpuResources()
	{
		const uint32_t buffers[] = { m_QuadVbo, m_QuadEbo, m_PosVbo, m_ScaleVbo, m_RotVbo, m_ColorVbo, m_OrigIdxVbo };
		glDeleteBuffers(sizeof(buffers) / sizeof(buffers[0]), buffers);
		if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
		if (m_ShTex) glDeleteTextures(1, &m_ShTex);

		m_Vao = m_QuadVbo = m_QuadEbo = 0;
		m_PosVbo = m_ScaleVbo = m_RotVbo = m_ColorVbo = m_OrigIdxVbo = 0;
		m_ShTex = 0;
		m_ShCoefCount = 0;
	}

}
