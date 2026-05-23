#include "pch.h"
#include "GizmoRenderer.h"

#include "FileReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <glm/gtc/constants.hpp>

namespace Engine {

	namespace {

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

		struct GizmoUniforms
		{
			glm::mat4 viewProj;
			glm::vec2 viewportSize;
			glm::vec2 _pad;
		};
		static_assert(sizeof(GizmoUniforms) == 64 + 16, "gizmo uniform layout drift");

		std::string LoadShaderSource(const std::string& relPath)
		{
			namespace fs = std::filesystem;
			const fs::path full = fs::path(ENGINE_ASSETS_DIR) / "shaders" / relPath;
			std::string src;
			FileReader reader(full.string());
			CORE_ASSERT(reader.Parse(src), "shader file missing");
			return src;
		}

		// Pick any unit vector orthogonal to `axis`. Used as the seed
		// direction for the ring's first chord; the rotate around `axis`
		// then sweeps the rest.
		glm::vec3 AnyOrthogonal(const glm::vec3& axis)
		{
			const glm::vec3 a = (std::abs(axis.x) < 0.5f)
				? glm::vec3(1.0f, 0.0f, 0.0f)
				: glm::vec3(0.0f, 1.0f, 0.0f);
			return glm::normalize(glm::cross(axis, a));
		}

	} // namespace


	GizmoRenderer::GizmoRenderer(WGPUContext& ctx)
		: m_Ctx(&ctx)
	{
		WGPUBufferDescriptor ud{};
		ud.label = SV("gizmo-uniform");
		ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		ud.size  = sizeof(GizmoUniforms);
		m_Uniform = wgpuDeviceCreateBuffer(m_Ctx->Device(), &ud);

		// Initial capacity — enough for a single full-feature gizmo
		// (translate arrows + rotate rings + scale cubes ~= 200 lines).
		EnsureLineBufferCapacity(256);
		CreatePipeline();
	}


	GizmoRenderer::~GizmoRenderer()
	{
		DestroyGpuResources();
	}


	void GizmoRenderer::EnsureLineBufferCapacity(size_t lineCount)
	{
		if (m_LineBuf && lineCount <= m_LineCap) return;

		// Round up to next power of two so growth is amortised.
		size_t cap = m_LineCap ? m_LineCap : 64;
		while (cap < lineCount) cap *= 2;

		if (m_LineBuf) {
			wgpuBufferRelease(m_LineBuf);
			m_LineBuf = nullptr;
		}

		WGPUBufferDescriptor bd{};
		bd.label = SV("gizmo-lines");
		bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
		bd.size  = cap * sizeof(LineEntry);
		m_LineBuf = wgpuDeviceCreateBuffer(m_Ctx->Device(), &bd);
		m_LineCap = cap;

		// Bind group needs to be rebuilt because the buffer handle changed.
		// CreatePipeline rebuilds it from scratch; if the pipeline already
		// exists we only need a new bind group.
		if (m_BGL) {
			if (m_BG) { wgpuBindGroupRelease(m_BG); m_BG = nullptr; }

			std::array<WGPUBindGroupEntry, 2> e{};
			e[0].binding = 0;
			e[0].buffer  = m_Uniform;
			e[0].size    = sizeof(GizmoUniforms);
			e[1].binding = 1;
			e[1].buffer  = m_LineBuf;
			e[1].size    = WGPU_WHOLE_SIZE;

			WGPUBindGroupDescriptor bgd{};
			bgd.label      = SV("gizmo-bg");
			bgd.layout     = m_BGL;
			bgd.entryCount = e.size();
			bgd.entries    = e.data();
			m_BG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
		}
	}


	void GizmoRenderer::CreatePipeline()
	{
		std::array<WGPUBindGroupLayoutEntry, 2> entries{};
		entries[0].binding    = 0;
		entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[0].buffer.type           = WGPUBufferBindingType_Uniform;
		entries[0].buffer.minBindingSize = sizeof(GizmoUniforms);

		entries[1].binding    = 1;
		entries[1].visibility = WGPUShaderStage_Vertex;
		entries[1].buffer.type           = WGPUBufferBindingType_ReadOnlyStorage;
		entries[1].buffer.minBindingSize = sizeof(LineEntry);

		WGPUBindGroupLayoutDescriptor bglDesc{};
		bglDesc.label      = SV("gizmo-bgl");
		bglDesc.entryCount = entries.size();
		bglDesc.entries    = entries.data();
		m_BGL = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &bglDesc);

		WGPUPipelineLayoutDescriptor plDesc{};
		plDesc.label                = SV("gizmo-pl");
		plDesc.bindGroupLayoutCount = 1;
		plDesc.bindGroupLayouts     = &m_BGL;
		m_PL = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &plDesc);

		const std::string src = LoadShaderSource("gizmo.wgsl");
		WGPUShaderSourceWGSL wgsl{};
		wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgsl.code        = SV(src.c_str());
		WGPUShaderModuleDescriptor sm{};
		sm.nextInChain = &wgsl.chain;
		sm.label       = SV("gizmo.wgsl");
		WGPUShaderModule shader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &sm);

		// Alpha-blend so dimmed (low-alpha) handles ghost through neatly.
		WGPUBlendComponent colorBlend{};
		colorBlend.srcFactor = WGPUBlendFactor_SrcAlpha;
		colorBlend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		colorBlend.operation = WGPUBlendOperation_Add;
		WGPUBlendState blend{};
		blend.color = colorBlend;
		blend.alpha = colorBlend;

		WGPUColorTargetState target{};
		target.format    = m_Ctx->SurfaceFormat();
		target.blend     = &blend;
		target.writeMask = WGPUColorWriteMask_All;

		WGPUFragmentState fs{};
		fs.module      = shader;
		fs.entryPoint  = SV("fs_main");
		fs.targetCount = 1;
		fs.targets     = &target;

		WGPURenderPipelineDescriptor pdesc{};
		pdesc.label                  = SV("gizmo-pipe");
		pdesc.layout                 = m_PL;
		pdesc.vertex.module          = shader;
		pdesc.vertex.entryPoint      = SV("vs_main");
		pdesc.primitive.topology     = WGPUPrimitiveTopology_TriangleList;
		pdesc.primitive.cullMode     = WGPUCullMode_None;
		pdesc.multisample.count      = 1;
		pdesc.multisample.mask       = 0xFFFFFFFFu;
		pdesc.fragment               = &fs;
		m_Pipe = wgpuDeviceCreateRenderPipeline(m_Ctx->Device(), &pdesc);

		wgpuShaderModuleRelease(shader);

		// First bind group binds the line buffer that EnsureLineBufferCapacity
		// already created in the constructor.
		std::array<WGPUBindGroupEntry, 2> bge{};
		bge[0].binding = 0;
		bge[0].buffer  = m_Uniform;
		bge[0].size    = sizeof(GizmoUniforms);
		bge[1].binding = 1;
		bge[1].buffer  = m_LineBuf;
		bge[1].size    = WGPU_WHOLE_SIZE;

		WGPUBindGroupDescriptor bgd{};
		bgd.label      = SV("gizmo-bg");
		bgd.layout     = m_BGL;
		bgd.entryCount = bge.size();
		bgd.entries    = bge.data();
		m_BG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
	}


	void GizmoRenderer::AddArrow(const glm::vec3& pivot, const glm::vec3& dir,
	                             float length, const glm::vec4& color,
	                             float thicknessPx)
	{
		const glm::vec3 d = glm::normalize(dir);
		const glm::vec3 tip = pivot + d * length;
		AddLine(pivot, tip, color, thicknessPx);

		// V-tip: two short lines from `tip` back along axis at ±30°.
		const float tipLen   = length * 0.18f;
		const float tipAng   = glm::radians(30.0f);
		// Pick any orthonormal "right" relative to axis dir.
		const glm::vec3 right = AnyOrthogonal(d);
		const glm::vec3 backDir = -d;

		const float ca = std::cos(tipAng);
		const float sa = std::sin(tipAng);
		const glm::vec3 v1 = glm::normalize(backDir * ca + right * sa);
		const glm::vec3 v2 = glm::normalize(backDir * ca - right * sa);
		AddLine(tip, tip + v1 * tipLen, color, thicknessPx);
		AddLine(tip, tip + v2 * tipLen, color, thicknessPx);
	}


	void GizmoRenderer::AddRing(const glm::vec3& center, const glm::vec3& axis,
	                            float radius, const glm::vec4& color,
	                            int segments, float thicknessPx)
	{
		const glm::vec3 u = AnyOrthogonal(axis);
		const glm::vec3 v = glm::normalize(glm::cross(axis, u));

		glm::vec3 prev = center + u * radius;
		for (int i = 1; i <= segments; ++i) {
			const float t = static_cast<float>(i) / static_cast<float>(segments);
			const float a = t * glm::two_pi<float>();
			const glm::vec3 next = center + (std::cos(a) * u + std::sin(a) * v) * radius;
			AddLine(prev, next, color, thicknessPx);
			prev = next;
		}
	}


	void GizmoRenderer::AddWireCube(const glm::vec3& center, float size,
	                                const glm::vec4& color, float thicknessPx)
	{
		const float h = size * 0.5f;
		const glm::vec3 c[8] = {
			center + glm::vec3(-h, -h, -h),  // 0
			center + glm::vec3( h, -h, -h),  // 1
			center + glm::vec3( h,  h, -h),  // 2
			center + glm::vec3(-h,  h, -h),  // 3
			center + glm::vec3(-h, -h,  h),  // 4
			center + glm::vec3( h, -h,  h),  // 5
			center + glm::vec3( h,  h,  h),  // 6
			center + glm::vec3(-h,  h,  h),  // 7
		};
		// 12 edges
		const int E[12][2] = {
			{0,1},{1,2},{2,3},{3,0}, // bottom
			{4,5},{5,6},{6,7},{7,4}, // top
			{0,4},{1,5},{2,6},{3,7}, // verticals
		};
		for (int i = 0; i < 12; ++i) {
			AddLine(c[E[i][0]], c[E[i][1]], color, thicknessPx);
		}
	}


	void GizmoRenderer::AddPlaneHandle(const glm::vec3& pivot,
	                                   const glm::vec3& axisA, const glm::vec3& axisB,
	                                   float size, const glm::vec4& color,
	                                   float thicknessPx)
	{
		const glm::vec3 a = pivot;
		const glm::vec3 b = pivot + axisA * size;
		const glm::vec3 c = pivot + axisA * size + axisB * size;
		const glm::vec3 d = pivot + axisB * size;
		AddLine(a, b, color, thicknessPx);
		AddLine(b, c, color, thicknessPx);
		AddLine(c, d, color, thicknessPx);
		AddLine(d, a, color, thicknessPx);
	}


	void GizmoRenderer::EncodeRender(WGPURenderPassEncoder pass,
	                                 const SPtr<Camera>& camera,
	                                 const glm::vec2& viewportSize)
	{
		if (m_Lines.empty()) return;

		EnsureLineBufferCapacity(m_Lines.size());

		const glm::mat4 vp = camera->GetProjectionMatrix() * camera->GetViewMatrix();
		GizmoUniforms gu{};
		gu.viewProj     = vp;
		gu.viewportSize = viewportSize;
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_Uniform, 0, &gu, sizeof(gu));

		// Upload visible lines.
		const size_t bytes = m_Lines.size() * sizeof(LineEntry);
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_LineBuf, 0, m_Lines.data(), bytes);

		wgpuRenderPassEncoderSetPipeline(pass, m_Pipe);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, m_BG, 0, nullptr);
		wgpuRenderPassEncoderDraw(pass, 6, static_cast<uint32_t>(m_Lines.size()), 0, 0);
	}


	void GizmoRenderer::DestroyGpuResources()
	{
		if (m_Pipe)    { wgpuRenderPipelineRelease(m_Pipe);    m_Pipe    = nullptr; }
		if (m_BG)      { wgpuBindGroupRelease(m_BG);           m_BG      = nullptr; }
		if (m_PL)      { wgpuPipelineLayoutRelease(m_PL);      m_PL      = nullptr; }
		if (m_BGL)     { wgpuBindGroupLayoutRelease(m_BGL);    m_BGL     = nullptr; }
		if (m_Uniform) { wgpuBufferRelease(m_Uniform);         m_Uniform = nullptr; }
		if (m_LineBuf) { wgpuBufferRelease(m_LineBuf);         m_LineBuf = nullptr; }
		m_LineCap = 0;
	}

}
