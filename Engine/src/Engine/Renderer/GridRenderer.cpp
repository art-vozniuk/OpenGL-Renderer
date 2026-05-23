#include "pch.h"
#include "GridRenderer.h"

#include "FileReader.h"

#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine {

	namespace {

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

		// Matches WGSL `Uniforms` in grid.wgsl.
		struct GridUniforms
		{
			glm::mat4 invViewProj;
			glm::vec4 cameraWorld;
			glm::vec4 params;
		};
		static_assert(sizeof(GridUniforms) == 64 + 16 + 16, "grid uniform layout drift");

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


	GridRenderer::GridRenderer(WGPUContext& ctx)
		: m_Ctx(&ctx)
	{
		WGPUBufferDescriptor ud{};
		ud.label = SV("grid-uniform");
		ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		ud.size  = sizeof(GridUniforms);
		m_Uniform = wgpuDeviceCreateBuffer(m_Ctx->Device(), &ud);

		CreatePipeline();
	}


	GridRenderer::~GridRenderer()
	{
		DestroyGpuResources();
	}


	void GridRenderer::CreatePipeline()
	{
		WGPUBindGroupLayoutEntry entry{};
		entry.binding    = 0;
		entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entry.buffer.type           = WGPUBufferBindingType_Uniform;
		entry.buffer.minBindingSize = sizeof(GridUniforms);

		WGPUBindGroupLayoutDescriptor bglDesc{};
		bglDesc.label      = SV("grid-bgl");
		bglDesc.entryCount = 1;
		bglDesc.entries    = &entry;
		m_BGL = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &bglDesc);

		WGPUPipelineLayoutDescriptor plDesc{};
		plDesc.label                = SV("grid-pl");
		plDesc.bindGroupLayoutCount = 1;
		plDesc.bindGroupLayouts     = &m_BGL;
		m_PL = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &plDesc);

		const std::string src = LoadShaderSource("grid.wgsl");
		WGPUShaderSourceWGSL wgsl{};
		wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgsl.code        = SV(src.c_str());
		WGPUShaderModuleDescriptor sm{};
		sm.nextInChain = &wgsl.chain;
		sm.label       = SV("grid.wgsl");
		WGPUShaderModule shader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &sm);

		// Premultiplied-alpha blend, same convention as the splat renderer
		// so the two passes compose without weighting the grid twice.
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
		pdesc.label                  = SV("grid-pipe");
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

		WGPUBindGroupEntry bge{};
		bge.binding = 0;
		bge.buffer  = m_Uniform;
		bge.size    = sizeof(GridUniforms);

		WGPUBindGroupDescriptor bgd{};
		bgd.label      = SV("grid-bg");
		bgd.layout     = m_BGL;
		bgd.entryCount = 1;
		bgd.entries    = &bge;
		m_BG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
	}


	void GridRenderer::EncodeRender(WGPURenderPassEncoder pass,
	                                const SPtr<Camera>& camera)
	{
		const glm::mat4& view = camera->GetViewMatrix();
		const glm::mat4& proj = camera->GetProjectionMatrix();
		const glm::mat4  vp   = proj * view;

		GridUniforms u{};
		u.invViewProj = glm::inverse(vp);
		// Camera world position = inverse(view) * (0,0,0,1).
		const glm::mat4 invView = glm::inverse(view);
		u.cameraWorld = glm::vec4(glm::vec3(invView[3]), 0.0f);
		u.params      = glm::vec4(m_MinorSpacing, m_MajorStride, m_FadeDist, m_Thickness);
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_Uniform, 0, &u, sizeof(u));

		wgpuRenderPassEncoderSetPipeline(pass, m_Pipe);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, m_BG, 0, nullptr);
		wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
	}


	void GridRenderer::DestroyGpuResources()
	{
		if (m_Pipe)    { wgpuRenderPipelineRelease(m_Pipe);    m_Pipe    = nullptr; }
		if (m_BG)      { wgpuBindGroupRelease(m_BG);           m_BG      = nullptr; }
		if (m_PL)      { wgpuPipelineLayoutRelease(m_PL);      m_PL      = nullptr; }
		if (m_BGL)     { wgpuBindGroupLayoutRelease(m_BGL);    m_BGL     = nullptr; }
		if (m_Uniform) { wgpuBufferRelease(m_Uniform);         m_Uniform = nullptr; }
	}

}
