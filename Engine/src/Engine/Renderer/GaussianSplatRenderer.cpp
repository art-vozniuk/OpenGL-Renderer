#include "pch.h"
#include "GaussianSplatRenderer.h"

#include "FileReader.h"

#include <array>
#include <cstring>
#include <filesystem>

#include <glm/gtc/type_ptr.hpp>

namespace Engine {

	namespace {

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

		// Render uniform: matches WGSL `Uniforms` in gsplat.wgsl.
		struct RenderUniforms
		{
			glm::mat4 view;
			glm::mat4 projection;
			glm::vec2 viewportSize;
			glm::vec2 _pad;
		};
		static_assert(sizeof(RenderUniforms) == 144, "render uniform layout drift");

		// Sort uniform: 36 bytes of payload (5x u32 + vec4f), but bound
		// with dynamic offset so each "config" must occupy a 256-byte
		// stride.
		constexpr size_t kSortUniformPayload = 48;
		constexpr size_t kSortUniformStride  = 256;
		constexpr int    kSortConfigInit  = 0;
		constexpr int    kSortConfigPass0 = 1;  // byte 0 (low), idxPing -> idxPong
		constexpr int    kSortConfigPass1 = 2;  // byte 1, idxPong -> idxPing
		constexpr int    kSortConfigPass2 = 3;  // byte 2, idxPing -> idxPong
		constexpr int    kSortConfigPass3 = 4;  // byte 3, idxPong -> idxPing
		constexpr int    kSortConfigCount = 5;

		struct SortUniform
		{
			float    viewRow2[4];
			uint32_t N;
			uint32_t digitShift;
			uint32_t swap;        // 0 -> read ping write pong, 1 -> reversed
			uint32_t numWg;       // ceil(N / 256)
		};
		static_assert(sizeof(SortUniform) == 32);

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
		CreateUniformBuffer();
		CreateSortResources();
		CreatePipelines();
	}


	GaussianSplatRenderer::~GaussianSplatRenderer()
	{
		DestroyGpuResources();
	}


	void GaussianSplatRenderer::CreateUniformBuffer()
	{
		// Render uniform — single block.
		WGPUBufferDescriptor rud{};
		rud.label = SV("gsplat-render-uniform");
		rud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		rud.size  = sizeof(RenderUniforms);
		m_RenderUniform = wgpuDeviceCreateBuffer(m_Ctx->Device(), &rud);

		// Sort uniform — 5 configs at 256-byte stride for dynamic offset.
		WGPUBufferDescriptor sud{};
		sud.label = SV("gsplat-sort-uniform");
		sud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		sud.size  = (uint64_t)kSortUniformStride * kSortConfigCount;
		m_SortUniform = wgpuDeviceCreateBuffer(m_Ctx->Device(), &sud);
	}


	void GaussianSplatRenderer::CreateSortResources()
	{
		// 256-entry digit-offset + digit-totals buffers are constant size.
		// wgHist / wgOffset depend on N -> created lazily in Upload().
		auto make256 = [&](const char* lbl) {
			WGPUBufferDescriptor d{};
			d.label = SV(lbl);
			d.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
			d.size  = 256u * sizeof(uint32_t);
			return wgpuDeviceCreateBuffer(m_Ctx->Device(), &d);
		};
		m_GlobalDigitOffset = make256("gsplat-globalDigitOffset");
		m_DigitTotals       = make256("gsplat-digitTotals");

		CreateTimestampResources();
	}


	void GaussianSplatRenderer::CreateTimestampResources()
	{
		m_TimestampsEnabled = m_Ctx->HasTimestampQueries();
		m_Metrics.gpuTimingsValid = m_TimestampsEnabled;
		if (!m_TimestampsEnabled) {
			INFO_CORE("gsplat: timestamp queries unavailable, perf overlay shows CPU-only timings");
			return;
		}

		// 6 slots, three (begin,end) pairs on three passes:
		//   [0,1] = init_depth pass            (sort *begin*)
		//   [2,3] = final byte-pass scatter    (sort *end*)
		//   [4,5] = render pass                (render *begin*+*end*)
		// We use pair-on-each-pass instead of "begin only" / "end only"
		// because Chrome / emdawnwebgpu rejects WGPU_QUERY_SET_INDEX_UNDEFINED
		// (UINT32_MAX) on the JS validator path even though Dawn-native
		// accepts it. All paired indices are valid `< count`.
		WGPUQuerySetDescriptor qd{};
		qd.label = SV("gsplat-perf-querySet");
		qd.type  = WGPUQueryType_Timestamp;
		qd.count = 6;
		m_QuerySet = wgpuDeviceCreateQuerySet(m_Ctx->Device(), &qd);

		// Resolve buffer is GPU-only (target of ResolveQuerySet).
		WGPUBufferDescriptor rd{};
		rd.label = SV("gsplat-perf-resolveBuf");
		rd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
		rd.size  = 6u * sizeof(uint64_t);
		m_TsResolveBuf = wgpuDeviceCreateBuffer(m_Ctx->Device(), &rd);

		// Mappable readback ring. Each slot is filled by CopyBufferToBuffer
		// from the resolve buffer; once GPU work is done we MapAsync the
		// slot and read u64 timestamps out.
		for (int i = 0; i < kTsRingSize; ++i) {
			WGPUBufferDescriptor md{};
			md.label = SV("gsplat-perf-mapBuf");
			md.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
			md.size  = 6u * sizeof(uint64_t);
			m_TsRing[i].mapBuf = wgpuDeviceCreateBuffer(m_Ctx->Device(), &md);
		}

		// Pre-bake the timestampWrites structs we hand out to pass
		// descriptors. These reference m_QuerySet directly — never
		// recreated at runtime, so address-stable for the whole renderer
		// lifetime.
		m_SortBeginTimestampWrites.querySet                  = m_QuerySet;
		m_SortBeginTimestampWrites.beginningOfPassWriteIndex = 0;
		m_SortBeginTimestampWrites.endOfPassWriteIndex       = 1;

		m_SortEndTimestampWrites.querySet                    = m_QuerySet;
		m_SortEndTimestampWrites.beginningOfPassWriteIndex   = 2;
		m_SortEndTimestampWrites.endOfPassWriteIndex         = 3;

		m_RenderTimestampWrites.querySet                     = m_QuerySet;
		m_RenderTimestampWrites.beginningOfPassWriteIndex    = 4;
		m_RenderTimestampWrites.endOfPassWriteIndex          = 5;

		INFO_CORE("gsplat: GPU timestamp queries enabled (6 slots, 3-deep readback ring)");
	}


	void GaussianSplatRenderer::DestroyTimestampResources()
	{
		if (m_QuerySet)     { wgpuQuerySetRelease(m_QuerySet);    m_QuerySet     = nullptr; }
		if (m_TsResolveBuf) { wgpuBufferRelease(m_TsResolveBuf);  m_TsResolveBuf = nullptr; }
		for (auto& s : m_TsRing) {
			if (s.mapBuf) { wgpuBufferRelease(s.mapBuf); s.mapBuf = nullptr; }
			s.resolved = false;
			s.mappingPending = false;
		}
		m_TimestampsEnabled = false;
	}


	void GaussianSplatRenderer::CreatePipelines()
	{
		// ---- Sort bind group layout (dynamic-offset uniform + 8 storage) ----
		std::array<WGPUBindGroupLayoutEntry, 9> sEntries{};

		sEntries[0].binding    = 0;
		sEntries[0].visibility = WGPUShaderStage_Compute;
		sEntries[0].buffer.type             = WGPUBufferBindingType_Uniform;
		sEntries[0].buffer.hasDynamicOffset = 1;
		sEntries[0].buffer.minBindingSize   = kSortUniformPayload;

		auto stor = [](uint32_t binding, WGPUBufferBindingType ty) {
			WGPUBindGroupLayoutEntry e{};
			e.binding    = binding;
			e.visibility = WGPUShaderStage_Compute;
			e.buffer.type           = ty;
			e.buffer.minBindingSize = 0;
			return e;
		};
		// 1=positions(read), 2=depths(rw), 3=idxPing(rw), 4=idxPong(rw),
		// 5=wgHist(rw, atomic), 6=wgOffset(rw), 7=globalDigitOffset(rw),
		// 8=digitTotals(rw)
		sEntries[1] = stor(1, WGPUBufferBindingType_ReadOnlyStorage);
		sEntries[2] = stor(2, WGPUBufferBindingType_Storage);
		sEntries[3] = stor(3, WGPUBufferBindingType_Storage);
		sEntries[4] = stor(4, WGPUBufferBindingType_Storage);
		sEntries[5] = stor(5, WGPUBufferBindingType_Storage);
		sEntries[6] = stor(6, WGPUBufferBindingType_Storage);
		sEntries[7] = stor(7, WGPUBufferBindingType_Storage);
		sEntries[8] = stor(8, WGPUBufferBindingType_Storage);

		WGPUBindGroupLayoutDescriptor sBglDesc{};
		sBglDesc.label      = SV("gsplat-sort-bgl");
		sBglDesc.entryCount = sEntries.size();
		sBglDesc.entries    = sEntries.data();
		m_SortBGL = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &sBglDesc);

		WGPUPipelineLayoutDescriptor sPlDesc{};
		sPlDesc.label                = SV("gsplat-sort-pl");
		sPlDesc.bindGroupLayoutCount = 1;
		sPlDesc.bindGroupLayouts     = &m_SortBGL;
		m_SortPL = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &sPlDesc);

		// ---- Sort compute pipelines ----
		const std::string sortSrc = LoadShaderSource("gsplat_sort.wgsl");
		WGPUShaderSourceWGSL sortWgsl{};
		sortWgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		sortWgsl.code        = SV(sortSrc.c_str());
		WGPUShaderModuleDescriptor smSort{};
		smSort.nextInChain = &sortWgsl.chain;
		smSort.label       = SV("gsplat_sort.wgsl");
		WGPUShaderModule sortShader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &smSort);

		auto MakeCompute = [&](const char* entry, const char* lbl) -> WGPUComputePipeline {
			WGPUComputePipelineDescriptor d{};
			d.label                    = SV(lbl);
			d.layout                   = m_SortPL;
			d.compute.module           = sortShader;
			d.compute.entryPoint       = SV(entry);
			return wgpuDeviceCreateComputePipeline(m_Ctx->Device(), &d);
		};
		m_PipeInit             = MakeCompute("cs_init_depth",        "gsplat-cs-init");
		m_PipeClearWgHist      = MakeCompute("cs_clear_wg_hist",     "gsplat-cs-clear-wghist");
		m_PipeWgHist           = MakeCompute("cs_wg_hist",           "gsplat-cs-wghist");
		m_PipeColumnScan       = MakeCompute("cs_column_scan",       "gsplat-cs-column-scan");
		m_PipeDigitOffsetScan  = MakeCompute("cs_digit_offset_scan", "gsplat-cs-digit-scan");
		m_PipeStableScatter    = MakeCompute("cs_stable_scatter",    "gsplat-cs-stable-scatter");
		wgpuShaderModuleRelease(sortShader);

		// ---- Render bind group layout ----
		std::array<WGPUBindGroupLayoutEntry, 6> rEntries{};

		rEntries[0].binding    = 0;
		rEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		rEntries[0].buffer.type           = WGPUBufferBindingType_Uniform;
		rEntries[0].buffer.minBindingSize = sizeof(RenderUniforms);

		auto rstor = [](uint32_t binding) {
			WGPUBindGroupLayoutEntry e{};
			e.binding    = binding;
			e.visibility = WGPUShaderStage_Vertex;
			e.buffer.type           = WGPUBufferBindingType_ReadOnlyStorage;
			e.buffer.minBindingSize = 0;
			return e;
		};
		rEntries[1] = rstor(1);  // positions
		rEntries[2] = rstor(2);  // scales
		rEntries[3] = rstor(3);  // rotations
		rEntries[4] = rstor(4);  // colors (as raw u32 array)
		rEntries[5] = rstor(5);  // sortedIndices

		WGPUBindGroupLayoutDescriptor rBglDesc{};
		rBglDesc.label      = SV("gsplat-render-bgl");
		rBglDesc.entryCount = rEntries.size();
		rBglDesc.entries    = rEntries.data();
		m_RenderBGL = wgpuDeviceCreateBindGroupLayout(m_Ctx->Device(), &rBglDesc);

		WGPUPipelineLayoutDescriptor rPlDesc{};
		rPlDesc.label                = SV("gsplat-render-pl");
		rPlDesc.bindGroupLayoutCount = 1;
		rPlDesc.bindGroupLayouts     = &m_RenderBGL;
		m_RenderPL = wgpuDeviceCreatePipelineLayout(m_Ctx->Device(), &rPlDesc);

		// ---- Render pipeline ----
		const std::string renderSrc = LoadShaderSource("gsplat.wgsl");
		WGPUShaderSourceWGSL rWgsl{};
		rWgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
		rWgsl.code        = SV(renderSrc.c_str());
		WGPUShaderModuleDescriptor smR{};
		smR.nextInChain = &rWgsl.chain;
		smR.label       = SV("gsplat.wgsl");
		WGPUShaderModule renderShader = wgpuDeviceCreateShaderModule(m_Ctx->Device(), &smR);

		WGPUBlendComponent colorBlend{};
		colorBlend.srcFactor = WGPUBlendFactor_One;
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
		fs.module      = renderShader;
		fs.entryPoint  = SV("fs_main");
		fs.targetCount = 1;
		fs.targets     = &target;

		WGPURenderPipelineDescriptor pdesc{};
		pdesc.label                  = SV("gsplat-render-pipe");
		pdesc.layout                 = m_RenderPL;
		pdesc.vertex.module          = renderShader;
		pdesc.vertex.entryPoint      = SV("vs_main");
		pdesc.primitive.topology     = WGPUPrimitiveTopology_TriangleList;
		pdesc.primitive.cullMode     = WGPUCullMode_None;
		pdesc.multisample.count      = 1;
		pdesc.multisample.mask       = 0xFFFFFFFFu;
		pdesc.fragment               = &fs;
		m_PipeRender = wgpuDeviceCreateRenderPipeline(m_Ctx->Device(), &pdesc);
		wgpuShaderModuleRelease(renderShader);
	}


	void GaussianSplatRenderer::Upload(const SplatData& data)
	{
		m_Count = data.Count();
		if (m_Count == 0) {
			WARN_CORE("GaussianSplatRenderer::Upload called with empty dataset");
			return;
		}

		// CPU side: pad vec3 -> vec4 for std430-friendly storage layout.
		std::vector<glm::vec4> posPadded(m_Count);
		std::vector<glm::vec4> scalePadded(m_Count);
		for (size_t i = 0; i < m_Count; ++i) {
			posPadded[i]   = glm::vec4(data.positions[i], 0.0f);
			scalePadded[i] = glm::vec4(data.scales[i],    0.0f);
		}
		// Colors: pack u8x4 into a single u32 (low byte = R as per WGSL
		// unpack4x8unorm convention). glm::u8vec4 already is RGBA in mem
		// little-endian, so a memcpy works.
		std::vector<uint32_t> colorsPacked(m_Count);
		std::memcpy(colorsPacked.data(), data.colors.data(), m_Count * sizeof(uint32_t));

		auto MakeStorageWith = [&](WGPUBuffer& dst, const void* src, size_t bytes,
		                           const char* lbl) {
			if (dst) wgpuBufferRelease(dst);
			WGPUBufferDescriptor d{};
			d.label = SV(lbl);
			d.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
			d.size  = bytes;
			dst = wgpuDeviceCreateBuffer(m_Ctx->Device(), &d);
			wgpuQueueWriteBuffer(m_Ctx->Queue(), dst, 0, src, bytes);
		};

		MakeStorageWith(m_Pos,   posPadded.data(),   m_Count * sizeof(glm::vec4), "gsplat-pos");
		MakeStorageWith(m_Scale, scalePadded.data(), m_Count * sizeof(glm::vec4), "gsplat-scale");
		MakeStorageWith(m_Rot,   data.rotations.data(), m_Count * sizeof(glm::vec4), "gsplat-rot");
		MakeStorageWith(m_Color, colorsPacked.data(),   m_Count * sizeof(uint32_t), "gsplat-color");

		// Per-frame sort scratch — sized to N, allocated once.
		auto MakeStorage = [&](WGPUBuffer& dst, size_t bytes, const char* lbl) {
			if (dst) wgpuBufferRelease(dst);
			WGPUBufferDescriptor d{};
			d.label = SV(lbl);
			d.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
			d.size  = bytes;
			dst = wgpuDeviceCreateBuffer(m_Ctx->Device(), &d);
		};
		MakeStorage(m_Depths,  m_Count * sizeof(uint32_t), "gsplat-depths");
		MakeStorage(m_IdxPing, m_Count * sizeof(uint32_t), "gsplat-idx-ping");
		MakeStorage(m_IdxPong, m_Count * sizeof(uint32_t), "gsplat-idx-pong");

		// num_wg = ceil(N / 256) — needed for wgHist / wgOffset sizing AND
		// passed to the shader via the sort uniform.
		const uint32_t numWg = (uint32_t)((m_Count + 255u) / 256u);
		const size_t wgHistBytes = (size_t)numWg * 256 * sizeof(uint32_t);
		MakeStorage(m_WgHist,   wgHistBytes, "gsplat-wgHist");
		MakeStorage(m_WgOffset, wgHistBytes, "gsplat-wgOffset");

		// Build sort + render bind groups now that all buffers exist.
		{
			std::array<WGPUBindGroupEntry, 9> e{};
			auto fill = [&](size_t i, uint32_t b, WGPUBuffer buf, uint64_t size) {
				e[i].binding = b; e[i].buffer = buf; e[i].size = size;
			};
			fill(0, 0, m_SortUniform,        kSortUniformPayload);
			fill(1, 1, m_Pos,                WGPU_WHOLE_SIZE);
			fill(2, 2, m_Depths,             WGPU_WHOLE_SIZE);
			fill(3, 3, m_IdxPing,            WGPU_WHOLE_SIZE);
			fill(4, 4, m_IdxPong,            WGPU_WHOLE_SIZE);
			fill(5, 5, m_WgHist,             WGPU_WHOLE_SIZE);
			fill(6, 6, m_WgOffset,           WGPU_WHOLE_SIZE);
			fill(7, 7, m_GlobalDigitOffset,  WGPU_WHOLE_SIZE);
			fill(8, 8, m_DigitTotals,        WGPU_WHOLE_SIZE);
			WGPUBindGroupDescriptor bgd{};
			bgd.label      = SV("gsplat-sort-bg");
			bgd.layout     = m_SortBGL;
			bgd.entryCount = e.size();
			bgd.entries    = e.data();
			if (m_SortBG) wgpuBindGroupRelease(m_SortBG);
			m_SortBG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
		}
		{
			std::array<WGPUBindGroupEntry, 6> e{};
			auto fill = [&](size_t i, uint32_t b, WGPUBuffer buf, uint64_t size) {
				e[i].binding = b; e[i].buffer = buf; e[i].size = size;
			};
			fill(0, 0, m_RenderUniform, sizeof(RenderUniforms));
			fill(1, 1, m_Pos,    WGPU_WHOLE_SIZE);
			fill(2, 2, m_Scale,  WGPU_WHOLE_SIZE);
			fill(3, 3, m_Rot,    WGPU_WHOLE_SIZE);
			fill(4, 4, m_Color,  WGPU_WHOLE_SIZE);
			// 4 byte-passes each ping<->pong flips the buffer; final state
			// after 4 swaps is back to IdxPing.
			fill(5, 5, m_IdxPing, WGPU_WHOLE_SIZE);
			WGPUBindGroupDescriptor bgd{};
			bgd.label      = SV("gsplat-render-bg");
			bgd.layout     = m_RenderBGL;
			bgd.entryCount = e.size();
			bgd.entries    = e.data();
			if (m_RenderBG) wgpuBindGroupRelease(m_RenderBG);
			m_RenderBG = wgpuDeviceCreateBindGroup(m_Ctx->Device(), &bgd);
		}

		INFO_CORE("GaussianSplatRenderer: uploaded {0} splats to GPU (storage layout)", (uint64_t)m_Count);
	}


	void GaussianSplatRenderer::EncodeSort(WGPUCommandEncoder encoder, const glm::mat4& viewMatrix)
	{
		if (m_Count == 0) return;

		const uint32_t numWg = ((uint32_t)m_Count + 255u) / 256u;

		// Build all 5 sort-uniform configs into a single contiguous block,
		// then one writeBuffer to push them.
		std::array<uint8_t, kSortUniformStride * kSortConfigCount> blob{};
		auto WriteCfg = [&](int slot, uint32_t shift, uint32_t swap) {
			SortUniform su{};
			su.viewRow2[0] = viewMatrix[0][2];
			su.viewRow2[1] = viewMatrix[1][2];
			su.viewRow2[2] = viewMatrix[2][2];
			su.viewRow2[3] = viewMatrix[3][2];
			su.N           = (uint32_t)m_Count;
			su.digitShift  = shift;
			su.swap        = swap;
			su.numWg       = numWg;
			std::memcpy(blob.data() + slot * kSortUniformStride, &su, sizeof(su));
		};
		WriteCfg(kSortConfigInit,  0,  0);
		WriteCfg(kSortConfigPass0, 0,  0);  // ping -> pong
		WriteCfg(kSortConfigPass1, 8,  1);  // pong -> ping
		WriteCfg(kSortConfigPass2, 16, 0);  // ping -> pong
		WriteCfg(kSortConfigPass3, 24, 1);  // pong -> ping
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_SortUniform, 0, blob.data(), blob.size());

		const uint32_t wgN = numWg;
		// numWg * 256 entries to clear; 256 threads/wg => numWg dispatches.
		const uint32_t wgClear = numWg;

		auto SetSortBG = [&](WGPUComputePassEncoder cp, uint32_t cfg) {
			uint32_t off = cfg * (uint32_t)kSortUniformStride;
			wgpuComputePassEncoderSetBindGroup(cp, 0, m_SortBG, 1, &off);
		};

		// One compute pass per dispatch. The WebGPU spec says writes from
		// dispatch K are visible to dispatch K+1 in the SAME pass, but
		// in practice both wgpu-native and Dawn's Metal backend have hit
		// memory-ordering bugs across dispatches inside a pass when
		// atomics + large storage buffers mix. Cross-pass ordering is
		// unambiguous — costs us a handful of pass starts (~µs each).
		auto Dispatch = [&](WGPUComputePipeline pipe, int cfg, uint32_t wgX,
		                    const char* label,
		                    const WGPUPassTimestampWrites* tw = nullptr) {
			WGPUComputePassDescriptor cpd{};
			cpd.label = SV(label);
			cpd.timestampWrites = tw;  // null when timestamps are disabled
			WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(encoder, &cpd);
			wgpuComputePassEncoderSetPipeline(cp, pipe);
			SetSortBG(cp, cfg);
			wgpuComputePassEncoderDispatchWorkgroups(cp, wgX, 1, 1);
			wgpuComputePassEncoderEnd(cp);
			wgpuComputePassEncoderRelease(cp);
		};

		// First sort dispatch records "begin sort" timestamp; the very last
		// dispatch (final scatter of byte-pass 3) records "end sort".
		const auto* sortBeginTw = m_TimestampsEnabled ? &m_SortBeginTimestampWrites : nullptr;
		const auto* sortEndTw   = m_TimestampsEnabled ? &m_SortEndTimestampWrites   : nullptr;

		Dispatch(m_PipeInit, kSortConfigInit, wgN, "gsplat-sort-init", sortBeginTw);

		auto OnePass = [&](int cfg, bool isLast) {
			// Stable workgroup-local radix:
			//   1) clear per-workgroup histogram (numWg * 256 entries)
			//   2) per-workgroup count (parallel)
			//   3a) column scan: 256 workgroups parallel-scan each digit's
			//       column of wgHist into wgOffset; emit digitTotals[d].
			//   3b) digit-offset scan: tiny single-thread prefix over
			//       digitTotals[256] -> globalDigitOffset[256].
			//   4) stable scatter using per-workgroup local rank.
			Dispatch(m_PipeClearWgHist,     cfg, wgClear, "gsplat-sort-clear-wghist");
			Dispatch(m_PipeWgHist,          cfg, wgN,     "gsplat-sort-wghist");
			Dispatch(m_PipeColumnScan,      cfg, 256,     "gsplat-sort-col-scan");
			Dispatch(m_PipeDigitOffsetScan, cfg, 1,       "gsplat-sort-digit-scan");
			Dispatch(m_PipeStableScatter,   cfg, wgN,     "gsplat-sort-stable-scatter",
			         isLast ? sortEndTw : nullptr);
		};
		OnePass(kSortConfigPass0, false);
		OnePass(kSortConfigPass1, false);
		OnePass(kSortConfigPass2, false);
		OnePass(kSortConfigPass3, true);

		m_FinalIsPing = true;  // 4 swaps -> back to IdxPing
	}


	void GaussianSplatRenderer::ResolveAndReadTimestamps(WGPUCommandEncoder encoder)
	{
		if (!m_TimestampsEnabled || m_Count == 0) return;

		// Resolve all 4 query slots to the GPU-only resolve buffer, then
		// copy into the next ring slot's MAP_READ buffer. mapAsync is
		// initiated AFTER the queue submit (caller does that via the next
		// frame's tick path) — here we only schedule GPU work.
		TsSlot& slot = m_TsRing[m_TsRingNext];
		if (slot.resolved || slot.mappingPending) {
			// Ring full — caller is producing frames faster than the map
			// callbacks resolve. Skip writing this frame's data; the
			// metric just shows the previous sample.
			return;
		}

		wgpuCommandEncoderResolveQuerySet(
			encoder, m_QuerySet, 0, 6, m_TsResolveBuf, 0);
		wgpuCommandEncoderCopyBufferToBuffer(
			encoder, m_TsResolveBuf, 0,
			slot.mapBuf, 0, 6u * sizeof(uint64_t));

		slot.resolved   = true;
		m_TsRingNext = (m_TsRingNext + 1) % kTsRingSize;
	}


	// ---- Async timestamp readback -----------------------------------------
	namespace {
		// Heap-allocated trampoline so the map callback knows which slot to
		// read and which renderer to push samples into. Freed inside the
		// callback. Necessary because WGPUBufferMapCallbackInfo only takes
		// raw void*'s; capturing a closure is out.
		struct MapTramp
		{
			GaussianSplatRenderer*               self;
			GaussianSplatRenderer::TsSlot*       slot;
		};

		void OnTimestampsMapped(WGPUMapAsyncStatus status,
		                        WGPUStringView /*msg*/,
		                        void* u1, void* /*u2*/)
		{
			MapTramp* t = static_cast<MapTramp*>(u1);
			if (!t) return;
			if (status == WGPUMapAsyncStatus_Success && t->slot && t->slot->mapBuf) {
				const uint64_t* ts =
					static_cast<const uint64_t*>(
						wgpuBufferGetConstMappedRange(t->slot->mapBuf, 0, 6u * sizeof(uint64_t)));
				if (ts) {
					// ts[0..5] layout (each pair = one pass's begin / end):
					//   0,1 = init_depth        (start of sort)
					//   2,3 = final scatter     (end of sort)
					//   4,5 = render pass
					// sort_total spans the whole sort: ts[3] - ts[0].
					if (ts[3] > ts[0] && ts[5] > ts[4]) {
						const double sortNs   = double(ts[3] - ts[0]);
						const double renderNs = double(ts[5] - ts[4]);
						auto& m = t->self->Metrics();
						m.gpuSortMs.Push(  float(sortNs   / 1.0e6));
						m.gpuRenderMs.Push(float(renderNs / 1.0e6));
						m.gpuTotalMs.Push( float((sortNs + renderNs) / 1.0e6));
					}
				}
				wgpuBufferUnmap(t->slot->mapBuf);
			}
			if (t->slot) {
				t->slot->resolved = false;
				t->slot->mappingPending = false;
			}
			delete t;
		}
	}


	void GaussianSplatRenderer::TickPerf()
	{
		// Forward any already-mapped readback slots into the metrics ring
		// (mapAsync callbacks fire from the JS event loop / Dawn process
		// events outside our control), and kick off mapAsync on any newly
		// resolved-but-not-yet-mapping slot so the next frame can pick up
		// the data once GPU is done.
		if (!m_TimestampsEnabled) return;

		for (int i = 0; i < kTsRingSize; ++i) {
			TsSlot& s = m_TsRing[i];
			if (s.resolved && !s.mappingPending) {
				s.mappingPending = true;
				MapTramp* t = new MapTramp{ this, &s };
				WGPUBufferMapCallbackInfo cbi{};
				cbi.mode      = WGPUCallbackMode_AllowSpontaneous;
				cbi.callback  = OnTimestampsMapped;
				cbi.userdata1 = t;
				(void)wgpuBufferMapAsync(s.mapBuf, WGPUMapMode_Read, 0,
				                         6u * sizeof(uint64_t), cbi);
			}
		}
	}


	void GaussianSplatRenderer::EncodeRender(WGPURenderPassEncoder pass,
	                                          const SPtr<Camera>& camera,
	                                          const glm::vec2& viewportSize)
	{
		if (m_Count == 0) return;

		RenderUniforms u{};
		u.view         = camera->GetViewMatrix();
		u.projection   = camera->GetProjectionMatrix();
		u.viewportSize = viewportSize;
		wgpuQueueWriteBuffer(m_Ctx->Queue(), m_RenderUniform, 0, &u, sizeof(u));

		wgpuRenderPassEncoderSetPipeline(pass, m_PipeRender);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, m_RenderBG, 0, nullptr);
		// Draw 6 verts/instance (two triangles), N instances. No index/vertex
		// buffer needed — corners are a const in the shader.
		wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)m_Count, 0, 0);
	}


	void GaussianSplatRenderer::DestroyGpuResources()
	{
		auto Drop = [](WGPUBuffer& b) { if (b) { wgpuBufferRelease(b); b = nullptr; } };
		Drop(m_Pos); Drop(m_Scale); Drop(m_Rot); Drop(m_Color);
		Drop(m_Depths); Drop(m_IdxPing); Drop(m_IdxPong);
		Drop(m_WgHist); Drop(m_WgOffset); Drop(m_GlobalDigitOffset); Drop(m_DigitTotals);
		Drop(m_RenderUniform); Drop(m_SortUniform);

		if (m_PipeInit)          { wgpuComputePipelineRelease(m_PipeInit);          m_PipeInit          = nullptr; }
		if (m_PipeClearWgHist)   { wgpuComputePipelineRelease(m_PipeClearWgHist);   m_PipeClearWgHist   = nullptr; }
		if (m_PipeWgHist)        { wgpuComputePipelineRelease(m_PipeWgHist);        m_PipeWgHist        = nullptr; }
		if (m_PipeColumnScan)      { wgpuComputePipelineRelease(m_PipeColumnScan);      m_PipeColumnScan      = nullptr; }
		if (m_PipeDigitOffsetScan) { wgpuComputePipelineRelease(m_PipeDigitOffsetScan); m_PipeDigitOffsetScan = nullptr; }
		if (m_PipeStableScatter) { wgpuComputePipelineRelease(m_PipeStableScatter); m_PipeStableScatter = nullptr; }
		if (m_PipeRender)        { wgpuRenderPipelineRelease(m_PipeRender);         m_PipeRender        = nullptr; }

		if (m_SortBG)     { wgpuBindGroupRelease(m_SortBG);             m_SortBG     = nullptr; }
		if (m_RenderBG)   { wgpuBindGroupRelease(m_RenderBG);           m_RenderBG   = nullptr; }
		if (m_SortBGL)    { wgpuBindGroupLayoutRelease(m_SortBGL);      m_SortBGL    = nullptr; }
		if (m_RenderBGL)  { wgpuBindGroupLayoutRelease(m_RenderBGL);    m_RenderBGL  = nullptr; }
		if (m_SortPL)     { wgpuPipelineLayoutRelease(m_SortPL);        m_SortPL     = nullptr; }
		if (m_RenderPL)   { wgpuPipelineLayoutRelease(m_RenderPL);      m_RenderPL   = nullptr; }

		DestroyTimestampResources();
	}

}
