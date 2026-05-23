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

		glm::vec3 AnyOrthogonal(const glm::vec3& axis)
		{
			const glm::vec3 a = (std::abs(axis.x) < 0.5f)
				? glm::vec3(1.0f, 0.0f, 0.0f)
				: glm::vec3(0.0f, 1.0f, 0.0f);
			return glm::normalize(glm::cross(axis, a));
		}

		WGPUBlendState AlphaBlend()
		{
			WGPUBlendComponent c{};
			c.srcFactor = WGPUBlendFactor_SrcAlpha;
			c.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
			c.operation = WGPUBlendOperation_Add;
			WGPUBlendState b{};
			b.color = c;
			b.alpha = c;
			return b;
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

		EnsureLineBufferCapacity(256);
		EnsureTriBufferCapacity(256);
		CreateLinePipeline();
		CreateTriPipeline();
	}


	GizmoRenderer::~GizmoRenderer()
	{
		DestroyGpuResources();
	}


	void GizmoRenderer::EnsureLineBufferCapacity(size_t lineCount)
	{
		if (m_LineBuf && lineCount <= m_LineCap) return;
		size_t cap = m_LineCap ? m_LineCap : 64;
		while (cap < lineCount) cap *= 2;

		if (m_LineBuf) { wgpuBufferRelease(m_LineBuf); m_LineBuf = nullptr; }
		WGPUBufferDescriptor bd{};
		bd.label = SV("gizmo-lines");
		bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
		bd.size  = cap * sizeof(LineEntry);
		m_LineBuf = wgpuDeviceCreateBuffer(m_Ctx->Device(), &bd);
		m_LineCap = cap;

		if (m_LineBGL) {
			if (m_LineBG) { wgpuBindGroupRelease(m_LineBG); m_LineBG = nullptr; }
			std::array<WGPUBindGroupEntry, 2> e{};
			e[0].binding = 0; e[0].buffer = m_Uniform; e[0].size = sizeof(GizmoUniforms);
			e[1].binding = 1; e[1].buffer = m_LineBuf; e[1].size = WGPU_WHOLE_SIZE;
			WGPUBindGroupDescriptor bgd{};
			bgd.label = SV("gizmo-line-bg");
			bgd.layout = m_LineBGL;
			bgd.entryCount = e.size();
			bgd.entries = e.data();
			m_LineBG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
		}
	}


	void GizmoRenderer::EnsureTriBufferCapacity(size_t triCount)
	{
		if (m_TriBuf && triCount <= m_TriCap) return;
		size_t cap = m_TriCap ? m_TriCap : 64;
		while (cap < triCount) cap *= 2;

		if (m_TriBuf) { wgpuBufferRelease(m_TriBuf); m_TriBuf = nullptr; }
		WGPUBufferDescriptor bd{};
		bd.label = SV("gizmo-tris");
		bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
		bd.size  = cap * sizeof(TriEntry);
		m_TriBuf = wgpuDeviceCreateBuffer(m_Ctx->Device(), &bd);
		m_TriCap = cap;

		if (m_TriBGL) {
			if (m_TriBG) { wgpuBindGroupRelease(m_TriBG); m_TriBG = nullptr; }
			std::array<WGPUBindGroupEntry, 2> e{};
			e[0].binding = 0; e[0].buffer = m_Uniform; e[0].size = sizeof(GizmoUniforms);
			e[1].binding = 1; e[1].buffer = m_TriBuf;  e[1].size = WGPU_WHOLE_SIZE;
			WGPUBindGroupDescriptor bgd{};
			bgd.label = SV("gizmo-tri-bg");
			bgd.layout = m_TriBGL;
			bgd.entryCount = e.size();
			bgd.entries = e.data();
			m_TriBG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
		}
	}


	void GizmoRenderer::CreateLinePipeline()
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
		bglDesc.label      = SV("gizmo-line-bgl");
		bglDesc.entryCount = entries.size();
		bglDesc.entries    = entries.data();
		m_LineBGL = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &bglDesc);

		WGPUPipelineLayoutDescriptor plDesc{};
		plDesc.label                = SV("gizmo-line-pl");
		plDesc.bindGroupLayoutCount = 1;
		plDesc.bindGroupLayouts     = &m_LineBGL;
		m_LinePL = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &plDesc);

		const std::string src = LoadShaderSource("gizmo.wgsl");
		WGPUShaderSourceWGSL wgsl{};
		wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgsl.code        = SV(src.c_str());
		WGPUShaderModuleDescriptor sm{};
		sm.nextInChain = &wgsl.chain;
		sm.label       = SV("gizmo.wgsl");
		WGPUShaderModule shader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &sm);

		WGPUBlendState blend = AlphaBlend();
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
		pdesc.label                  = SV("gizmo-line-pipe");
		pdesc.layout                 = m_LinePL;
		pdesc.vertex.module          = shader;
		pdesc.vertex.entryPoint      = SV("vs_main");
		pdesc.primitive.topology     = WGPUPrimitiveTopology_TriangleList;
		pdesc.primitive.cullMode     = WGPUCullMode_None;
		pdesc.multisample.count      = 1;
		pdesc.multisample.mask       = 0xFFFFFFFFu;
		pdesc.fragment               = &fs;
		m_LinePipe = wgpuDeviceCreateRenderPipeline(m_Ctx->Device(), &pdesc);
		wgpuShaderModuleRelease(shader);

		std::array<WGPUBindGroupEntry, 2> bge{};
		bge[0].binding = 0; bge[0].buffer = m_Uniform; bge[0].size = sizeof(GizmoUniforms);
		bge[1].binding = 1; bge[1].buffer = m_LineBuf; bge[1].size = WGPU_WHOLE_SIZE;
		WGPUBindGroupDescriptor bgd{};
		bgd.label      = SV("gizmo-line-bg");
		bgd.layout     = m_LineBGL;
		bgd.entryCount = bge.size();
		bgd.entries    = bge.data();
		m_LineBG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
	}


	void GizmoRenderer::CreateTriPipeline()
	{
		std::array<WGPUBindGroupLayoutEntry, 2> entries{};
		entries[0].binding    = 0;
		entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[0].buffer.type           = WGPUBufferBindingType_Uniform;
		entries[0].buffer.minBindingSize = sizeof(GizmoUniforms);
		entries[1].binding    = 1;
		entries[1].visibility = WGPUShaderStage_Vertex;
		entries[1].buffer.type           = WGPUBufferBindingType_ReadOnlyStorage;
		entries[1].buffer.minBindingSize = sizeof(TriEntry);

		WGPUBindGroupLayoutDescriptor bglDesc{};
		bglDesc.label      = SV("gizmo-tri-bgl");
		bglDesc.entryCount = entries.size();
		bglDesc.entries    = entries.data();
		m_TriBGL = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &bglDesc);

		WGPUPipelineLayoutDescriptor plDesc{};
		plDesc.label                = SV("gizmo-tri-pl");
		plDesc.bindGroupLayoutCount = 1;
		plDesc.bindGroupLayouts     = &m_TriBGL;
		m_TriPL = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &plDesc);

		const std::string src = LoadShaderSource("gizmo_tri.wgsl");
		WGPUShaderSourceWGSL wgsl{};
		wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgsl.code        = SV(src.c_str());
		WGPUShaderModuleDescriptor sm{};
		sm.nextInChain = &wgsl.chain;
		sm.label       = SV("gizmo_tri.wgsl");
		WGPUShaderModule shader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &sm);

		WGPUBlendState blend = AlphaBlend();
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
		pdesc.label                  = SV("gizmo-tri-pipe");
		pdesc.layout                 = m_TriPL;
		pdesc.vertex.module          = shader;
		pdesc.vertex.entryPoint      = SV("vs_main");
		pdesc.primitive.topology     = WGPUPrimitiveTopology_TriangleList;
		pdesc.primitive.cullMode     = WGPUCullMode_None;
		pdesc.multisample.count      = 1;
		pdesc.multisample.mask       = 0xFFFFFFFFu;
		pdesc.fragment               = &fs;
		m_TriPipe = wgpuDeviceCreateRenderPipeline(m_Ctx->Device(), &pdesc);
		wgpuShaderModuleRelease(shader);

		std::array<WGPUBindGroupEntry, 2> bge{};
		bge[0].binding = 0; bge[0].buffer = m_Uniform; bge[0].size = sizeof(GizmoUniforms);
		bge[1].binding = 1; bge[1].buffer = m_TriBuf;  bge[1].size = WGPU_WHOLE_SIZE;
		WGPUBindGroupDescriptor bgd{};
		bgd.label      = SV("gizmo-tri-bg");
		bgd.layout     = m_TriBGL;
		bgd.entryCount = bge.size();
		bgd.entries    = bge.data();
		m_TriBG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
	}


	void GizmoRenderer::AddArrow(const glm::vec3& pivot, const glm::vec3& dir,
	                             float length, const glm::vec4& color,
	                             float thicknessPx)
	{
		const glm::vec3 d = glm::normalize(dir);
		const glm::vec3 tip = pivot + d * length;
		AddLine(pivot, tip, color, thicknessPx);

		const float tipLen   = length * 0.18f;
		const float tipAng   = glm::radians(30.0f);
		const glm::vec3 right = AnyOrthogonal(d);
		const glm::vec3 backDir = -d;
		const float ca = std::cos(tipAng);
		const float sa = std::sin(tipAng);
		const glm::vec3 v1 = glm::normalize(backDir * ca + right * sa);
		const glm::vec3 v2 = glm::normalize(backDir * ca - right * sa);
		AddLine(tip, tip + v1 * tipLen, color, thicknessPx);
		AddLine(tip, tip + v2 * tipLen, color, thicknessPx);
	}


	void GizmoRenderer::AddArrowHead(const glm::vec3& tip, const glm::vec3& axisDir,
	                                 float headLen, float headWidth,
	                                 const glm::vec3& cameraPos, const glm::vec4& color)
	{
		const glm::vec3 d = glm::normalize(axisDir);
		const glm::vec3 base = tip - d * headLen;
		// Billboard "sideways" — perpendicular to both axis and view direction
		// so the arrowhead always presents broadside to the camera.
		const glm::vec3 toCam = glm::normalize(cameraPos - base);
		glm::vec3 side = glm::cross(d, toCam);
		const float sl = glm::length(side);
		if (sl < 1e-5f) side = AnyOrthogonal(d);
		else            side = side / sl;
		const glm::vec3 baseL = base - side * headWidth * 0.5f;
		const glm::vec3 baseR = base + side * headWidth * 0.5f;
		// Two-triangle "diamond" so the head looks solid from either side.
		AddTri(tip, baseR, baseL, color);
		const glm::vec3 perp = glm::cross(d, side);
		const glm::vec3 baseT = base + perp * headWidth * 0.35f;
		const glm::vec3 baseB = base - perp * headWidth * 0.35f;
		AddTri(tip, baseT, baseB, color);
	}


	void GizmoRenderer::AddRing(const glm::vec3& center, const glm::vec3& axis,
	                            float radius, const glm::vec4& color,
	                            int segments, float thicknessPx)
	{
		AddArc(center, axis, radius, 0.0f, glm::two_pi<float>(),
		       color, segments, thicknessPx);
	}


	void GizmoRenderer::AddArc(const glm::vec3& center, const glm::vec3& axis,
	                           float radius, float startRad, float endRad,
	                           const glm::vec4& color, int segments,
	                           float thicknessPx)
	{
		const glm::vec3 u = AnyOrthogonal(axis);
		const glm::vec3 v = glm::normalize(glm::cross(axis, u));
		const float span = endRad - startRad;
		glm::vec3 prev = center + (std::cos(startRad) * u + std::sin(startRad) * v) * radius;
		for (int i = 1; i <= segments; ++i) {
			const float t = static_cast<float>(i) / static_cast<float>(segments);
			const float a = startRad + span * t;
			const glm::vec3 next = center + (std::cos(a) * u + std::sin(a) * v) * radius;
			AddLine(prev, next, color, thicknessPx);
			prev = next;
		}
	}


	void GizmoRenderer::AddDisk(const glm::vec3& center, float radius,
	                            const glm::vec3& cameraPos, const glm::vec4& color,
	                            int segments)
	{
		const glm::vec3 viewDir = glm::normalize(center - cameraPos);
		const glm::vec3 u = AnyOrthogonal(viewDir);
		const glm::vec3 v = glm::normalize(glm::cross(viewDir, u));
		glm::vec3 prev = center + u * radius;
		for (int i = 1; i <= segments; ++i) {
			const float t = static_cast<float>(i) / static_cast<float>(segments);
			const float a = t * glm::two_pi<float>();
			const glm::vec3 next = center + (std::cos(a) * u + std::sin(a) * v) * radius;
			AddTri(center, prev, next, color);
			prev = next;
		}
	}


	void GizmoRenderer::AddWireCube(const glm::vec3& center, float size,
	                                const glm::vec4& color, float thicknessPx)
	{
		const float h = size * 0.5f;
		const glm::vec3 c[8] = {
			center + glm::vec3(-h, -h, -h), center + glm::vec3( h, -h, -h),
			center + glm::vec3( h,  h, -h), center + glm::vec3(-h,  h, -h),
			center + glm::vec3(-h, -h,  h), center + glm::vec3( h, -h,  h),
			center + glm::vec3( h,  h,  h), center + glm::vec3(-h,  h,  h),
		};
		const int E[12][2] = {
			{0,1},{1,2},{2,3},{3,0},
			{4,5},{5,6},{6,7},{7,4},
			{0,4},{1,5},{2,6},{3,7},
		};
		for (int i = 0; i < 12; ++i) AddLine(c[E[i][0]], c[E[i][1]], color, thicknessPx);
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
		if (m_Lines.empty() && m_Tris.empty()) return;

		const glm::mat4 vp = camera->GetProjectionMatrix() * camera->GetViewMatrix();
		GizmoUniforms gu{};
		gu.viewProj     = vp;
		gu.viewportSize = viewportSize;
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_Uniform, 0, &gu, sizeof(gu));

		// Tris first so lines overlay on top (look "in front" of arrowheads).
		if (!m_Tris.empty()) {
			EnsureTriBufferCapacity(m_Tris.size());
			wgpuQueueWriteBuffer(m_Ctx->Queue(), m_TriBuf, 0,
			                     m_Tris.data(), m_Tris.size() * sizeof(TriEntry));
			wgpuRenderPassEncoderSetPipeline(pass, m_TriPipe);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, m_TriBG, 0, nullptr);
			wgpuRenderPassEncoderDraw(pass, 3, static_cast<uint32_t>(m_Tris.size()), 0, 0);
		}

		if (!m_Lines.empty()) {
			EnsureLineBufferCapacity(m_Lines.size());
			wgpuQueueWriteBuffer(m_Ctx->Queue(), m_LineBuf, 0,
			                     m_Lines.data(), m_Lines.size() * sizeof(LineEntry));
			wgpuRenderPassEncoderSetPipeline(pass, m_LinePipe);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, m_LineBG, 0, nullptr);
			wgpuRenderPassEncoderDraw(pass, 6, static_cast<uint32_t>(m_Lines.size()), 0, 0);
		}
	}


	void GizmoRenderer::DestroyGpuResources()
	{
		if (m_LinePipe) { wgpuRenderPipelineRelease(m_LinePipe); m_LinePipe = nullptr; }
		if (m_LineBG)   { wgpuBindGroupRelease(m_LineBG);        m_LineBG   = nullptr; }
		if (m_LinePL)   { wgpuPipelineLayoutRelease(m_LinePL);   m_LinePL   = nullptr; }
		if (m_LineBGL)  { wgpuBindGroupLayoutRelease(m_LineBGL); m_LineBGL  = nullptr; }
		if (m_LineBuf)  { wgpuBufferRelease(m_LineBuf);          m_LineBuf  = nullptr; }
		m_LineCap = 0;

		if (m_TriPipe) { wgpuRenderPipelineRelease(m_TriPipe); m_TriPipe = nullptr; }
		if (m_TriBG)   { wgpuBindGroupRelease(m_TriBG);        m_TriBG   = nullptr; }
		if (m_TriPL)   { wgpuPipelineLayoutRelease(m_TriPL);   m_TriPL   = nullptr; }
		if (m_TriBGL)  { wgpuBindGroupLayoutRelease(m_TriBGL); m_TriBGL  = nullptr; }
		if (m_TriBuf)  { wgpuBufferRelease(m_TriBuf);          m_TriBuf  = nullptr; }
		m_TriCap = 0;

		if (m_Uniform) { wgpuBufferRelease(m_Uniform); m_Uniform = nullptr; }
	}

}
