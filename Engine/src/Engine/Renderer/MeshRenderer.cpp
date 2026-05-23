#include "pch.h"
#include "MeshRenderer.h"

#include "FileReader.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>

namespace Engine {

	namespace {

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

		struct PbrUniforms
		{
			glm::mat4 view;
			glm::mat4 projection;
			glm::mat4 model;
			glm::mat4 normalMat;
			glm::vec4 lightDir;
			glm::vec4 lightColor;
			glm::vec4 ambient;
			glm::vec4 baseColorFactor;
			glm::vec4 mrFactor;
			glm::vec4 flags;
		};
		static_assert(sizeof(PbrUniforms) == 4*64 + 6*16, "pbr uniform layout drift");

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


	MeshRenderer::MeshRenderer(WGPUContext& ctx)
		: m_Ctx(&ctx)
	{
		CreatePipeline();

		WGPUSamplerDescriptor sd{};
		sd.label = SV("pbr-sampler");
		sd.addressModeU = WGPUAddressMode_Repeat;
		sd.addressModeV = WGPUAddressMode_Repeat;
		sd.addressModeW = WGPUAddressMode_Repeat;
		sd.magFilter = WGPUFilterMode_Linear;
		sd.minFilter = WGPUFilterMode_Linear;
		sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
		sd.maxAnisotropy = 1;
		m_Sampler = wgpuDeviceCreateSampler(m_Ctx->Device(), &sd);

		// Persistent 1x1 dummies — referenced by materials lacking a given texture.
		uint8_t white[4]  = {255,255,255,255};
		uint8_t flatN[4]  = {128,128,255,255};
		uint8_t mr[4]     = {255,255,255,255};
		WGPUTextureViewDescriptor vd{};
		vd.format = WGPUTextureFormat_RGBA8Unorm;
		vd.dimension = WGPUTextureViewDimension_2D;
		vd.mipLevelCount = 1;
		vd.arrayLayerCount = 1;
		m_DummyWhiteTex   = CreateRgba8Texture(white, 1, 1, "pbr-dummy-white");
		m_DummyWhiteView  = wgpuTextureCreateView(m_DummyWhiteTex, &vd);
		m_DummyNormalTex  = CreateRgba8Texture(flatN, 1, 1, "pbr-dummy-normal");
		m_DummyNormalView = wgpuTextureCreateView(m_DummyNormalTex, &vd);
		m_DummyMRTex      = CreateRgba8Texture(mr, 1, 1, "pbr-dummy-mr");
		m_DummyMRView     = wgpuTextureCreateView(m_DummyMRTex, &vd);
	}


	MeshRenderer::~MeshRenderer()
	{
		DestroyAll();
	}


	void MeshRenderer::CreatePipeline()
	{
		std::array<WGPUBindGroupLayoutEntry, 5> entries{};
		entries[0].binding    = 0;
		entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[0].buffer.type           = WGPUBufferBindingType_Uniform;
		entries[0].buffer.minBindingSize = sizeof(PbrUniforms);

		entries[1].binding    = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = WGPUSamplerBindingType_Filtering;

		auto tex = [](uint32_t binding) {
			WGPUBindGroupLayoutEntry e{};
			e.binding    = binding;
			e.visibility = WGPUShaderStage_Fragment;
			e.texture.sampleType    = WGPUTextureSampleType_Float;
			e.texture.viewDimension = WGPUTextureViewDimension_2D;
			e.texture.multisampled  = 0;
			return e;
		};
		entries[2] = tex(2);
		entries[3] = tex(3);
		entries[4] = tex(4);

		WGPUBindGroupLayoutDescriptor bglDesc{};
		bglDesc.label      = SV("pbr-bgl");
		bglDesc.entryCount = entries.size();
		bglDesc.entries    = entries.data();
		m_BGL = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &bglDesc);

		WGPUPipelineLayoutDescriptor plDesc{};
		plDesc.label                = SV("pbr-pl");
		plDesc.bindGroupLayoutCount = 1;
		plDesc.bindGroupLayouts     = &m_BGL;
		m_PL = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &plDesc);

		const std::string src = LoadShaderSource("pbr.wgsl");
		WGPUShaderSourceWGSL wgsl{};
		wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgsl.code        = SV(src.c_str());
		WGPUShaderModuleDescriptor sm{};
		sm.nextInChain = &wgsl.chain;
		sm.label       = SV("pbr.wgsl");
		WGPUShaderModule shader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &sm);

		std::array<WGPUVertexAttribute, 4> attrs{};
		attrs[0].shaderLocation = 0;
		attrs[0].format = WGPUVertexFormat_Float32x3;
		attrs[0].offset = 0;
		attrs[1].shaderLocation = 1;
		attrs[1].format = WGPUVertexFormat_Float32x3;
		attrs[1].offset = 0;
		attrs[2].shaderLocation = 2;
		attrs[2].format = WGPUVertexFormat_Float32x2;
		attrs[2].offset = 0;
		attrs[3].shaderLocation = 3;
		attrs[3].format = WGPUVertexFormat_Float32x4;
		attrs[3].offset = 0;

		std::array<WGPUVertexBufferLayout, 4> vbLayouts{};
		vbLayouts[0].arrayStride = sizeof(glm::vec3);
		vbLayouts[0].stepMode    = WGPUVertexStepMode_Vertex;
		vbLayouts[0].attributeCount = 1;
		vbLayouts[0].attributes  = &attrs[0];

		vbLayouts[1].arrayStride = sizeof(glm::vec3);
		vbLayouts[1].stepMode    = WGPUVertexStepMode_Vertex;
		vbLayouts[1].attributeCount = 1;
		vbLayouts[1].attributes  = &attrs[1];

		vbLayouts[2].arrayStride = sizeof(glm::vec2);
		vbLayouts[2].stepMode    = WGPUVertexStepMode_Vertex;
		vbLayouts[2].attributeCount = 1;
		vbLayouts[2].attributes  = &attrs[2];

		vbLayouts[3].arrayStride = sizeof(glm::vec4);
		vbLayouts[3].stepMode    = WGPUVertexStepMode_Vertex;
		vbLayouts[3].attributeCount = 1;
		vbLayouts[3].attributes  = &attrs[3];

		WGPUBlendComponent cb{};
		cb.srcFactor = WGPUBlendFactor_SrcAlpha;
		cb.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		cb.operation = WGPUBlendOperation_Add;
		WGPUBlendState blend{};
		blend.color = cb;
		blend.alpha = cb;

		WGPUColorTargetState target{};
		target.format    = m_Ctx->SurfaceFormat();
		target.blend     = &blend;
		target.writeMask = WGPUColorWriteMask_All;

		WGPUFragmentState fs{};
		fs.module      = shader;
		fs.entryPoint  = SV("fs_main");
		fs.targetCount = 1;
		fs.targets     = &target;

		WGPUDepthStencilState ds{};
		ds.format            = DepthFormat();
		ds.depthWriteEnabled = WGPUOptionalBool_True;
		ds.depthCompare      = WGPUCompareFunction_Less;
		// D24+ has no stencil — defaults (Always, Keep, Keep, Keep) are fine.
		ds.stencilFront.compare     = WGPUCompareFunction_Always;
		ds.stencilFront.failOp      = WGPUStencilOperation_Keep;
		ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
		ds.stencilFront.passOp      = WGPUStencilOperation_Keep;
		ds.stencilBack              = ds.stencilFront;
		ds.stencilReadMask  = 0xFFFFFFFFu;
		ds.stencilWriteMask = 0xFFFFFFFFu;

		WGPURenderPipelineDescriptor pdesc{};
		pdesc.label                = SV("pbr-pipe");
		pdesc.layout               = m_PL;
		pdesc.vertex.module        = shader;
		pdesc.vertex.entryPoint    = SV("vs_main");
		pdesc.vertex.bufferCount   = vbLayouts.size();
		pdesc.vertex.buffers       = vbLayouts.data();
		pdesc.primitive.topology   = WGPUPrimitiveTopology_TriangleList;
		pdesc.primitive.cullMode   = WGPUCullMode_None;
		pdesc.primitive.frontFace  = WGPUFrontFace_CCW;
		pdesc.depthStencil         = &ds;
		pdesc.multisample.count    = 1;
		pdesc.multisample.mask     = 0xFFFFFFFFu;
		pdesc.fragment             = &fs;
		m_Pipeline = wgpuDeviceCreateRenderPipeline(m_Ctx->Device(), &pdesc);
		wgpuShaderModuleRelease(shader);
	}


	WGPUTexture MeshRenderer::CreateRgba8Texture(const uint8_t* pixels, int w, int h,
	                                             const char* label)
	{
		WGPUTextureDescriptor td{};
		td.label  = SV(label);
		td.usage  = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		td.dimension = WGPUTextureDimension_2D;
		td.size.width  = (uint32_t)w;
		td.size.height = (uint32_t)h;
		td.size.depthOrArrayLayers = 1;
		td.format = WGPUTextureFormat_RGBA8Unorm;
		td.mipLevelCount = 1;
		td.sampleCount   = 1;
		WGPUTexture tex = wgpuDeviceCreateTexture(m_Ctx->Device(), &td);

		WGPUTexelCopyTextureInfo dst{};
		dst.texture = tex;
		dst.aspect  = WGPUTextureAspect_All;

		WGPUTexelCopyBufferLayout layout{};
		layout.bytesPerRow  = (uint32_t)w * 4;
		layout.rowsPerImage = (uint32_t)h;

		WGPUExtent3D ext{};
		ext.width = (uint32_t)w;
		ext.height = (uint32_t)h;
		ext.depthOrArrayLayers = 1;

		wgpuQueueWriteTexture(m_Ctx->Queue(), &dst, pixels, (size_t)w * h * 4, &layout, &ext);
		return tex;
	}


	void MeshRenderer::Upload(const MeshData& data)
	{
		DestroyMeshData();

		m_AABB.min = data.aabbMin;
		m_AABB.max = data.aabbMax;
		m_AABB.valid = data.aabbValid;

		const size_t numMats = std::max<size_t>(1, data.materials.size());
		m_Materials.resize(numMats);

		WGPUTextureViewDescriptor vd{};
		vd.format = WGPUTextureFormat_RGBA8Unorm;
		vd.dimension = WGPUTextureViewDimension_2D;
		vd.mipLevelCount = 1;
		vd.arrayLayerCount = 1;

		for (size_t i = 0; i < numMats; ++i) {
			GpuMaterial& gm = m_Materials[i];
			const MeshMaterial src = (i < data.materials.size()) ? data.materials[i] : MeshMaterial{};
			gm.baseColorFactor = src.baseColorFactor;
			gm.metallicFactor  = src.metallicFactor;
			gm.roughnessFactor = src.roughnessFactor;

			WGPUBufferDescriptor ud{};
			ud.label = SV("pbr-mat-uniform");
			ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
			ud.size  = sizeof(PbrUniforms);
			gm.uniform = wgpuDeviceCreateBuffer(m_Ctx->Device(), &ud);

			if (!src.baseColorPixels.empty()) {
				gm.baseColorTex  = CreateRgba8Texture(src.baseColorPixels.data(),
				                                     src.baseColorWidth, src.baseColorHeight,
				                                     "pbr-baseColor");
				gm.baseColorView = wgpuTextureCreateView(gm.baseColorTex, &vd);
				gm.hasBaseColorTex = true;
			}
			if (!src.normalPixels.empty()) {
				gm.normalTex  = CreateRgba8Texture(src.normalPixels.data(),
				                                  src.normalWidth, src.normalHeight,
				                                  "pbr-normal");
				gm.normalView = wgpuTextureCreateView(gm.normalTex, &vd);
				gm.hasNormalTex = true;
			}
			if (!src.mrPixels.empty()) {
				gm.mrTex  = CreateRgba8Texture(src.mrPixels.data(),
				                              src.mrWidth, src.mrHeight,
				                              "pbr-mr");
				gm.mrView = wgpuTextureCreateView(gm.mrTex, &vd);
				gm.hasMRTex = true;
			}

			std::array<WGPUBindGroupEntry, 5> be{};
			be[0].binding = 0; be[0].buffer = gm.uniform; be[0].size = sizeof(PbrUniforms);
			be[1].binding = 1; be[1].sampler = m_Sampler;
			be[2].binding = 2; be[2].textureView = gm.baseColorView ? gm.baseColorView : m_DummyWhiteView;
			be[3].binding = 3; be[3].textureView = gm.normalView    ? gm.normalView    : m_DummyNormalView;
			be[4].binding = 4; be[4].textureView = gm.mrView        ? gm.mrView        : m_DummyMRView;

			WGPUBindGroupDescriptor bgd{};
			bgd.label      = SV("pbr-mat-bg");
			bgd.layout     = m_BGL;
			bgd.entryCount = be.size();
			bgd.entries    = be.data();
			gm.bindGroup = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
		}

		m_Primitives.reserve(data.primitives.size());
		for (const auto& src : data.primitives) {
			GpuPrimitive gp{};
			gp.indexCount    = (uint32_t)src.indices.size();
			gp.vertexCount   = (uint32_t)src.positions.size();
			gp.materialIndex = (src.materialIndex >= 0 && (size_t)src.materialIndex < m_Materials.size())
			                   ? src.materialIndex : 0;

			const uint32_t N = gp.vertexCount;
			const size_t posBytes = N * sizeof(glm::vec3);
			const size_t nrmBytes = N * sizeof(glm::vec3);
			const size_t uvBytes  = N * sizeof(glm::vec2);
			const size_t tanBytes = N * sizeof(glm::vec4);
			const size_t total    = posBytes + nrmBytes + uvBytes + tanBytes;

			WGPUBufferDescriptor vbd{};
			vbd.label = SV("pbr-vbo");
			vbd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
			vbd.size  = total;
			gp.vbo = wgpuDeviceCreateBuffer(m_Ctx->Device(), &vbd);

			size_t off = 0;
			wgpuQueueWriteBuffer(m_Ctx->Queue(), gp.vbo, off, src.positions.data(), posBytes); off += posBytes;
			wgpuQueueWriteBuffer(m_Ctx->Queue(), gp.vbo, off, src.normals.data(),   nrmBytes); off += nrmBytes;
			wgpuQueueWriteBuffer(m_Ctx->Queue(), gp.vbo, off, src.uvs.data(),       uvBytes);  off += uvBytes;
			wgpuQueueWriteBuffer(m_Ctx->Queue(), gp.vbo, off, src.tangents.data(),  tanBytes);

			WGPUBufferDescriptor ibd{};
			ibd.label = SV("pbr-ibo");
			ibd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
			ibd.size  = src.indices.size() * sizeof(uint32_t);
			gp.ibo = wgpuDeviceCreateBuffer(m_Ctx->Device(), &ibd);
			wgpuQueueWriteBuffer(m_Ctx->Queue(), gp.ibo, 0, src.indices.data(), ibd.size);

			m_Primitives.push_back(gp);
		}

		INFO_CORE("MeshRenderer: uploaded {0} primitives, {1} materials",
		          (uint64_t)m_Primitives.size(), (uint64_t)m_Materials.size());
	}


	void MeshRenderer::EncodeRender(WGPURenderPassEncoder pass,
	                                const SPtr<Camera>& camera,
	                                const glm::vec2& /*viewportSize*/,
	                                const glm::vec3& lightDirWorld,
	                                const glm::vec3& lightColor,
	                                const glm::vec3& ambient)
	{
		if (m_Primitives.empty() || m_Materials.empty()) return;

		const glm::mat4& view = camera->GetViewMatrix();
		const glm::mat4& proj = camera->GetProjectionMatrix();

		PbrUniforms baseU{};
		baseU.view       = view;
		baseU.projection = proj;
		baseU.model      = m_ModelMatrix;
		glm::mat3 nm3 = glm::transpose(glm::inverse(glm::mat3(m_ModelMatrix)));
		baseU.normalMat[0] = glm::vec4(nm3[0], 0.0f);
		baseU.normalMat[1] = glm::vec4(nm3[1], 0.0f);
		baseU.normalMat[2] = glm::vec4(nm3[2], 0.0f);
		baseU.normalMat[3] = glm::vec4(0,0,0,1);
		baseU.lightDir   = glm::vec4(glm::normalize(lightDirWorld), 0.0f);
		baseU.lightColor = glm::vec4(lightColor, 0.0f);
		baseU.ambient    = glm::vec4(ambient, 0.0f);

		wgpuRenderPassEncoderSetPipeline(pass, m_Pipeline);

		for (const auto& gp : m_Primitives) {
			const GpuMaterial& gm = m_Materials[(size_t)gp.materialIndex];

			PbrUniforms uu = baseU;
			uu.baseColorFactor = gm.baseColorFactor;
			uu.mrFactor        = glm::vec4(gm.metallicFactor, gm.roughnessFactor, 0.0f, 0.0f);
			uu.flags = glm::vec4(
				gm.hasBaseColorTex ? 1.0f : 0.0f,
				gm.hasNormalTex    ? 1.0f : 0.0f,
				gm.hasMRTex        ? 1.0f : 0.0f,
				0.0f);
			wgpuQueueWriteBuffer(m_Ctx->Queue(), gm.uniform, 0, &uu, sizeof(uu));

			wgpuRenderPassEncoderSetBindGroup(pass, 0, gm.bindGroup, 0, nullptr);

			const uint64_t Nv = (uint64_t)gp.vertexCount;
			const uint64_t posBytesTotal = Nv * 12ull;
			const uint64_t nrmBytesTotal = Nv * 12ull;
			const uint64_t uvBytesTotal  = Nv * 8ull;
			const uint64_t tanBytesTotal = Nv * 16ull;

			wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gp.vbo, 0, posBytesTotal);
			wgpuRenderPassEncoderSetVertexBuffer(pass, 1, gp.vbo, posBytesTotal, nrmBytesTotal);
			wgpuRenderPassEncoderSetVertexBuffer(pass, 2, gp.vbo, posBytesTotal + nrmBytesTotal, uvBytesTotal);
			wgpuRenderPassEncoderSetVertexBuffer(pass, 3, gp.vbo, posBytesTotal + nrmBytesTotal + uvBytesTotal, tanBytesTotal);

			wgpuRenderPassEncoderSetIndexBuffer(pass, gp.ibo, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderDrawIndexed(pass, gp.indexCount, 1, 0, 0, 0);
		}
	}


	void MeshRenderer::DestroyMeshData()
	{
		for (auto& gp : m_Primitives) {
			if (gp.vbo) wgpuBufferRelease(gp.vbo);
			if (gp.ibo) wgpuBufferRelease(gp.ibo);
		}
		m_Primitives.clear();

		for (auto& gm : m_Materials) {
			if (gm.bindGroup)     wgpuBindGroupRelease(gm.bindGroup);
			if (gm.uniform)       wgpuBufferRelease(gm.uniform);
			if (gm.baseColorView) wgpuTextureViewRelease(gm.baseColorView);
			if (gm.baseColorTex)  wgpuTextureRelease(gm.baseColorTex);
			if (gm.normalView)    wgpuTextureViewRelease(gm.normalView);
			if (gm.normalTex)     wgpuTextureRelease(gm.normalTex);
			if (gm.mrView)        wgpuTextureViewRelease(gm.mrView);
			if (gm.mrTex)         wgpuTextureRelease(gm.mrTex);
		}
		m_Materials.clear();
	}


	void MeshRenderer::DestroyAll()
	{
		DestroyMeshData();

		if (m_DummyWhiteView)  { wgpuTextureViewRelease(m_DummyWhiteView);  m_DummyWhiteView  = nullptr; }
		if (m_DummyWhiteTex)   { wgpuTextureRelease(m_DummyWhiteTex);       m_DummyWhiteTex   = nullptr; }
		if (m_DummyNormalView) { wgpuTextureViewRelease(m_DummyNormalView); m_DummyNormalView = nullptr; }
		if (m_DummyNormalTex)  { wgpuTextureRelease(m_DummyNormalTex);      m_DummyNormalTex  = nullptr; }
		if (m_DummyMRView)     { wgpuTextureViewRelease(m_DummyMRView);     m_DummyMRView     = nullptr; }
		if (m_DummyMRTex)      { wgpuTextureRelease(m_DummyMRTex);          m_DummyMRTex      = nullptr; }

		if (m_Sampler)  { wgpuSamplerRelease(m_Sampler);  m_Sampler  = nullptr; }
		if (m_Pipeline) { wgpuRenderPipelineRelease(m_Pipeline); m_Pipeline = nullptr; }
		if (m_PL)       { wgpuPipelineLayoutRelease(m_PL);       m_PL       = nullptr; }
		if (m_BGL)      { wgpuBindGroupLayoutRelease(m_BGL);     m_BGL      = nullptr; }
	}

}
