#include "pch.h"
#include "GaussianSplatRenderer.h"

#include "FileReader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>

#include <glm/gtc/type_ptr.hpp>

namespace Engine {

	namespace {

		// 5-second window at 60 fps for the rolling-max stats panel.
		constexpr size_t kHistoryFrames = 300;

		using Clock = std::chrono::steady_clock;
		inline float ElapsedMs(Clock::time_point t0, Clock::time_point t1)
		{
			return std::chrono::duration<float, std::milli>(t1 - t0).count();
		}

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

		// CPU-side mirror of the WGSL `Uniforms` struct. std::array of floats
		// because GLM matrices are column-major / 16-byte aligned which
		// matches WGSL's std140-ish uniform layout.
		struct UniformsCPU
		{
			glm::mat4 view;
			glm::mat4 projection;
			glm::vec2 viewportSize;
			glm::vec2 _pad;
		};
		static_assert(sizeof(UniformsCPU) == 144, "uniform layout drifted");

		// Read the WGSL shader source from disk relative to the engine
		// asset root. Throws via assertion on missing file.
		std::string LoadShaderSource(const std::string& relPath)
		{
			namespace fs = std::filesystem;
			const fs::path full = fs::path(ENGINE_ASSETS_DIR) / "shaders" / relPath;
			std::string src;
			FileReader reader(full.string());
			CORE_ASSERT(reader.Parse(src), "shader file missing");
			return src;
		}

	} // namespace


	GaussianSplatRenderer::GaussianSplatRenderer(WGPUContext& ctx)
		: m_Ctx(&ctx)
	{
		CreateQuadGeometry();
		CreateInstanceBuffers();
		CreateUniformBuffer();
		CreatePipeline();
		m_History.assign(kHistoryFrames, PerfStats{});
	}


	GaussianSplatRenderer::~GaussianSplatRenderer()
	{
		DestroyGpuResources();
	}


	void GaussianSplatRenderer::CreateQuadGeometry()
	{
		// Unit quad corners in [-1, +1]^2; scaled per-instance by the
		// vertex shader to bound the projected ellipse.
		static const float kQuadVerts[] = {
			-1.0f, -1.0f,
			 1.0f, -1.0f,
			 1.0f,  1.0f,
			-1.0f,  1.0f,
		};
		static const uint32_t kQuadIdx[] = { 0, 1, 2, 0, 2, 3 };

		WGPUBufferDescriptor vDesc{};
		vDesc.label = SV("gsplat-quad-verts");
		vDesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
		vDesc.size  = sizeof(kQuadVerts);
		m_QuadVerts = wgpuDeviceCreateBuffer(m_Ctx->Device(), &vDesc);
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_QuadVerts, 0, kQuadVerts, sizeof(kQuadVerts));

		WGPUBufferDescriptor iDesc{};
		iDesc.label = SV("gsplat-quad-idx");
		iDesc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
		iDesc.size  = sizeof(kQuadIdx);
		m_QuadIndices = wgpuDeviceCreateBuffer(m_Ctx->Device(), &iDesc);
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_QuadIndices, 0, kQuadIdx, sizeof(kQuadIdx));
	}


	void GaussianSplatRenderer::CreateInstanceBuffers()
	{
		// Buffers grow lazily in Upload(); we just create empty handles
		// here so the pipeline can reference them. If Upload() never runs
		// the renderer simply draws zero instances.
	}


	void GaussianSplatRenderer::CreateUniformBuffer()
	{
		WGPUBufferDescriptor uDesc{};
		uDesc.label = SV("gsplat-uniforms");
		uDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		uDesc.size  = sizeof(UniformsCPU);
		m_UniformBuf = wgpuDeviceCreateBuffer(m_Ctx->Device(), &uDesc);

		// Bind group layout: just one uniform buffer at @binding(0).
		WGPUBindGroupLayoutEntry e{};
		e.binding    = 0;
		e.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		e.buffer.type           = WGPUBufferBindingType_Uniform;
		e.buffer.minBindingSize = sizeof(UniformsCPU);

		WGPUBindGroupLayoutDescriptor bglDesc{};
		bglDesc.label      = SV("gsplat-bgl");
		bglDesc.entryCount = 1;
		bglDesc.entries    = &e;
		m_BindLayout = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &bglDesc);

		WGPUBindGroupEntry be{};
		be.binding = 0;
		be.buffer  = m_UniformBuf;
		be.size    = sizeof(UniformsCPU);

		WGPUBindGroupDescriptor bgDesc{};
		bgDesc.label      = SV("gsplat-bg");
		bgDesc.layout     = m_BindLayout;
		bgDesc.entryCount = 1;
		bgDesc.entries    = &be;
		m_BindGroup = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgDesc);

		WGPUPipelineLayoutDescriptor plDesc{};
		plDesc.label                = SV("gsplat-pl");
		plDesc.bindGroupLayoutCount = 1;
		plDesc.bindGroupLayouts     = &m_BindLayout;
		m_PipeLayout = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &plDesc);
	}


	void GaussianSplatRenderer::CreatePipeline()
	{
		const std::string shaderSrc = LoadShaderSource("gsplat.wgsl");

		WGPUShaderSourceWGSL wgsl{};
		wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgsl.code        = SV(shaderSrc.c_str());
		WGPUShaderModuleDescriptor smDesc{};
		smDesc.nextInChain = &wgsl.chain;
		smDesc.label       = SV("gsplat.wgsl");
		WGPUShaderModule shader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &smDesc);

		// Vertex buffer layouts: 5 separate buffers, one shared (corner)
		// stepping per-vertex, four stepping per-instance. WebGPU calls
		// these "VertexBufferLayout" — equivalent to GL's vertexAttribDivisor.
		WGPUVertexAttribute aCorner{};
		aCorner.format         = WGPUVertexFormat_Float32x2;
		aCorner.offset         = 0;
		aCorner.shaderLocation = 0;
		WGPUVertexBufferLayout cornerLayout{};
		cornerLayout.arrayStride    = sizeof(float) * 2;
		cornerLayout.stepMode       = WGPUVertexStepMode_Vertex;
		cornerLayout.attributeCount = 1;
		cornerLayout.attributes     = &aCorner;

		WGPUVertexAttribute aPos{};
		aPos.format         = WGPUVertexFormat_Float32x3;
		aPos.shaderLocation = 1;
		WGPUVertexBufferLayout posLayout{};
		posLayout.arrayStride    = sizeof(float) * 3;
		posLayout.stepMode       = WGPUVertexStepMode_Instance;
		posLayout.attributeCount = 1;
		posLayout.attributes     = &aPos;

		WGPUVertexAttribute aScale{};
		aScale.format         = WGPUVertexFormat_Float32x3;
		aScale.shaderLocation = 2;
		WGPUVertexBufferLayout scaleLayout{};
		scaleLayout.arrayStride    = sizeof(float) * 3;
		scaleLayout.stepMode       = WGPUVertexStepMode_Instance;
		scaleLayout.attributeCount = 1;
		scaleLayout.attributes     = &aScale;

		WGPUVertexAttribute aRot{};
		aRot.format         = WGPUVertexFormat_Float32x4;
		aRot.shaderLocation = 3;
		WGPUVertexBufferLayout rotLayout{};
		rotLayout.arrayStride    = sizeof(float) * 4;
		rotLayout.stepMode       = WGPUVertexStepMode_Instance;
		rotLayout.attributeCount = 1;
		rotLayout.attributes     = &aRot;

		WGPUVertexAttribute aColor{};
		aColor.format         = WGPUVertexFormat_Unorm8x4;  // matches u8vec4 normalised
		aColor.shaderLocation = 4;
		WGPUVertexBufferLayout colorLayout{};
		colorLayout.arrayStride    = sizeof(uint8_t) * 4;
		colorLayout.stepMode       = WGPUVertexStepMode_Instance;
		colorLayout.attributeCount = 1;
		colorLayout.attributes     = &aColor;

		WGPUVertexBufferLayout buffers[5] = {
			cornerLayout, posLayout, scaleLayout, rotLayout, colorLayout
		};

		// Fragment / blend: alpha-over with premultiplied source.
		WGPUBlendComponent colorBlend{};
		colorBlend.srcFactor = WGPUBlendFactor_One;
		colorBlend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		colorBlend.operation = WGPUBlendOperation_Add;
		WGPUBlendComponent alphaBlend = colorBlend;
		WGPUBlendState blend{};
		blend.color = colorBlend;
		blend.alpha = alphaBlend;

		WGPUColorTargetState target{};
		target.format    = m_Ctx->SurfaceFormat();
		target.blend     = &blend;
		target.writeMask = WGPUColorWriteMask_All;

		WGPUFragmentState fs{};
		fs.module      = shader;
		fs.entryPoint  = SV("fs_main");
		fs.targetCount = 1;
		fs.targets     = &target;

		WGPURenderPipelineDescriptor desc{};
		desc.label                  = SV("gsplat-pipeline");
		desc.layout                 = m_PipeLayout;
		desc.vertex.module          = shader;
		desc.vertex.entryPoint      = SV("vs_main");
		desc.vertex.bufferCount     = 5;
		desc.vertex.buffers         = buffers;
		desc.primitive.topology     = WGPUPrimitiveTopology_TriangleList;
		desc.primitive.cullMode     = WGPUCullMode_None;
		desc.multisample.count      = 1;
		desc.multisample.mask       = 0xFFFFFFFFu;
		desc.fragment               = &fs;

		m_Pipeline = wgpuDeviceCreateRenderPipeline(m_Ctx->Device(), &desc);
		wgpuShaderModuleRelease(shader);
	}


	void GaussianSplatRenderer::Upload(const SplatData& data)
	{
		m_Count = data.Count();
		if (m_Count == 0) {
			WARN_CORE("GaussianSplatRenderer::Upload called with empty dataset");
			return;
		}

		m_Positions = data.positions;
		m_Scales    = data.scales;
		m_Rotations = data.rotations;
		m_Colors    = data.colors;

		m_SortIndices.resize(m_Count);
		m_SortIndicesScratch.resize(m_Count);
		m_SortKeys.resize(m_Count);
		m_SortKeysScratch.resize(m_Count);
		m_ScratchVec3.resize(m_Count);
		m_ScratchVec4.resize(m_Count);
		m_ScratchRgba.resize(m_Count);
		m_Depths.resize(m_Count);
		std::iota(m_SortIndices.begin(), m_SortIndices.end(), uint32_t{0});

		const size_t vec3Bytes = m_Count * sizeof(glm::vec3);
		const size_t vec4Bytes = m_Count * sizeof(glm::vec4);
		const size_t rgbaBytes = m_Count * sizeof(glm::u8vec4);

		// Recreate per-instance buffers sized to the dataset. The previous
		// content (if any) is just released — same as glBufferData on GL.
		auto MakeVB = [&](WGPUBuffer& buf, size_t bytes, const char* lbl) {
			if (buf) wgpuBufferRelease(buf);
			WGPUBufferDescriptor d{};
			d.label = SV(lbl);
			d.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
			d.size  = bytes;
			buf = wgpuDeviceCreateBuffer(m_Ctx->Device(), &d);
		};

		MakeVB(m_PosBuf,   vec3Bytes, "gsplat-pos");
		MakeVB(m_ScaleBuf, vec3Bytes, "gsplat-scale");
		MakeVB(m_RotBuf,   vec4Bytes, "gsplat-rot");
		MakeVB(m_ColorBuf, rgbaBytes, "gsplat-color");

		auto Q = m_Ctx->Queue();
		wgpuQueueWriteBuffer(Q, m_PosBuf,   0, data.positions.data(), vec3Bytes);
		wgpuQueueWriteBuffer(Q, m_ScaleBuf, 0, data.scales.data(),    vec3Bytes);
		wgpuQueueWriteBuffer(Q, m_RotBuf,   0, data.rotations.data(), vec4Bytes);
		wgpuQueueWriteBuffer(Q, m_ColorBuf, 0, data.colors.data(),    rgbaBytes);

		m_SortValid = false;
		INFO_CORE("GaussianSplatRenderer: uploaded {0} splats to GPU", (uint64_t)m_Count);
	}


	bool GaussianSplatRenderer::NeedsResort(const glm::mat4& viewMatrix) const
	{
		if (!m_SortValid) return true;
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

		// View-space depth as sortable uint32 (sign-bit flip trick — same
		// as the GL renderer). Sorting ascending uint == ascending float.
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

		// LSD radix sort — 4 byte-wide passes. Same as the GL impl.
		std::iota(m_SortIndices.begin(), m_SortIndices.end(), uint32_t{0});
		for (int byteIdx = 0; byteIdx < 4; ++byteIdx) {
			const int shift = byteIdx * 8;
			uint32_t buckets[256] = {0};
			for (size_t i = 0; i < m_Count; ++i)
				++buckets[(m_SortKeys[i] >> shift) & 0xFFu];
			uint32_t sum = 0;
			for (int bk = 0; bk < 256; ++bk) {
				uint32_t cnt = buckets[bk];
				buckets[bk] = sum;
				sum += cnt;
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

		// Reshuffle each attribute into scratch + upload to its GPU buffer.
		auto Q = m_Ctx->Queue();

		for (size_t i = 0; i < m_Count; ++i) m_ScratchVec3[i] = m_Positions[m_SortIndices[i]];
		auto tPosReshuffle = Clock::now();
		wgpuQueueWriteBuffer(Q, m_PosBuf, 0, m_ScratchVec3.data(),
		                     m_Count * sizeof(glm::vec3));

		for (size_t i = 0; i < m_Count; ++i) m_ScratchVec3[i] = m_Scales[m_SortIndices[i]];
		wgpuQueueWriteBuffer(Q, m_ScaleBuf, 0, m_ScratchVec3.data(),
		                     m_Count * sizeof(glm::vec3));

		for (size_t i = 0; i < m_Count; ++i) m_ScratchVec4[i] = m_Rotations[m_SortIndices[i]];
		wgpuQueueWriteBuffer(Q, m_RotBuf, 0, m_ScratchVec4.data(),
		                     m_Count * sizeof(glm::vec4));

		for (size_t i = 0; i < m_Count; ++i) m_ScratchRgba[i] = m_Colors[m_SortIndices[i]];
		wgpuQueueWriteBuffer(Q, m_ColorBuf, 0, m_ScratchRgba.data(),
		                     m_Count * sizeof(glm::u8vec4));

		auto tUploaded = Clock::now();

		m_LastFrame.sortMs      = ElapsedMs(tStart, tSorted);
		m_LastFrame.reshuffleMs = ElapsedMs(tSorted, tPosReshuffle);
		m_LastFrame.uploadMs    = ElapsedMs(tPosReshuffle, tUploaded);

		m_LastSortView = viewMatrix;
		m_SortValid = true;
	}


	void GaussianSplatRenderer::Render(WGPURenderPassEncoder pass,
	                                   const SPtr<Camera>& camera,
	                                   const glm::vec2& viewportSize)
	{
		if (m_Count == 0) return;
		m_LastFrame = PerfStats{};

		const glm::mat4& view = camera->GetViewMatrix();

		const glm::vec3 fPrev(-m_LastObservedView[0][2], -m_LastObservedView[1][2], -m_LastObservedView[2][2]);
		const glm::vec3 fNow (-view[0][2],               -view[1][2],               -view[2][2]);
		const glm::vec3 pPrev = -glm::vec3(m_LastObservedView[3]);
		const glm::vec3 pNow  = -glm::vec3(view[3]);
		const bool movingNow = glm::dot(fPrev, fNow) < 0.99999f
		                    || glm::length(pPrev - pNow) > 1e-4f;

		if (NeedsResort(view)) Sort(view);

		m_LastObservedView   = view;
		m_WasMovingLastFrame = movingNow;

		// Push camera + viewport to the uniform buffer.
		UniformsCPU u{};
		u.view         = view;
		u.projection   = camera->GetProjectionMatrix();
		u.viewportSize = viewportSize;
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_UniformBuf, 0, &u, sizeof(u));

		auto tDrawStart = Clock::now();
		wgpuRenderPassEncoderSetPipeline(pass, m_Pipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, m_BindGroup, 0, nullptr);

		// Vertex buffers, slot 0..4 (corner, pos, scale, rot, color).
		wgpuRenderPassEncoderSetVertexBuffer(pass, 0, m_QuadVerts, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderSetVertexBuffer(pass, 1, m_PosBuf,    0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderSetVertexBuffer(pass, 2, m_ScaleBuf,  0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderSetVertexBuffer(pass, 3, m_RotBuf,    0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderSetVertexBuffer(pass, 4, m_ColorBuf,  0, WGPU_WHOLE_SIZE);

		wgpuRenderPassEncoderSetIndexBuffer(pass, m_QuadIndices,
		                                    WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
		wgpuRenderPassEncoderDrawIndexed(pass,
		                                 /*indexCount=*/6,
		                                 /*instanceCount=*/(uint32_t)m_Count,
		                                 /*firstIndex=*/0,
		                                 /*baseVertex=*/0,
		                                 /*firstInstance=*/0);
		// drawMs measures CPU-side encode time only without an explicit
		// GPU sync; getting real GPU time needs a timestamp query
		// (compute follow-up).
		m_LastFrame.drawMs = ElapsedMs(tDrawStart, Clock::now());

		m_History[m_HistoryHead] = m_LastFrame;
		m_HistoryHead = (m_HistoryHead + 1) % m_History.size();
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
		auto Drop = [](WGPUBuffer& b)             { if (b) { wgpuBufferRelease(b); b = nullptr; } };
		Drop(m_QuadVerts);
		Drop(m_QuadIndices);
		Drop(m_PosBuf);
		Drop(m_ScaleBuf);
		Drop(m_RotBuf);
		Drop(m_ColorBuf);
		Drop(m_UniformBuf);
		if (m_Pipeline)   { wgpuRenderPipelineRelease(m_Pipeline);     m_Pipeline   = nullptr; }
		if (m_PipeLayout) { wgpuPipelineLayoutRelease(m_PipeLayout);   m_PipeLayout = nullptr; }
		if (m_BindGroup)  { wgpuBindGroupRelease(m_BindGroup);         m_BindGroup  = nullptr; }
		if (m_BindLayout) { wgpuBindGroupLayoutRelease(m_BindLayout);  m_BindLayout = nullptr; }
	}

}
