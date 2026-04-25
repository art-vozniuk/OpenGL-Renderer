#include "pch.h"
#include "Renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace Engine {

	namespace {

		struct State {
			WGPUContext*          ctx       = nullptr;
			SPtr<Camera>          camera;
			WGPUContext::Frame    frame;
			WGPURenderPassEncoder pass      = nullptr;

			// Screenshot side-channel: filled by RequestScreenshot, consumed
			// in EndScene.
			std::string           screenshotPath;
			bool                  screenshotPending = false;
		};
		State g{};

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
		}

		// WebGPU CopyTextureToBuffer requires bytesPerRow to be 256-aligned.
		uint32_t AlignBytesPerRow(uint32_t width, uint32_t bytesPerPixel)
		{
			const uint32_t bpr = width * bytesPerPixel;
			constexpr uint32_t kAlign = 256;
			return (bpr + kAlign - 1) & ~(kAlign - 1);
		}

		void OnBufferMapped(WGPUMapAsyncStatus /*status*/, WGPUStringView /*msg*/,
		                    void* u1, void*)
		{
			*static_cast<volatile bool*>(u1) = true;
		}

		// Apple surfaces are typically BGRA8Unorm — swizzle to RGBA for PNG.
		void SwizzleBGRAtoRGBA(uint8_t* px, size_t pixelCount)
		{
			for (size_t i = 0; i < pixelCount; ++i) {
				uint8_t b = px[4*i + 0];
				uint8_t r = px[4*i + 2];
				px[4*i + 0] = r;
				px[4*i + 2] = b;
			}
		}

		// Encode the surface->staging copy into the in-flight frame encoder
		// and queue the post-submit map+save in EndScene. Single-shot.
		WGPUBuffer EmitScreenshotCopy(uint32_t& outBpr)
		{
			const uint32_t w = g.ctx->Width();
			const uint32_t h = g.ctx->Height();
			const uint32_t bpp = 4;
			const uint32_t paddedBpr = AlignBytesPerRow(w, bpp);

			WGPUBufferDescriptor bd{};
			bd.label = SV("screenshot-staging");
			bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
			bd.size  = (uint64_t)paddedBpr * h;
			WGPUBuffer staging = wgpuDeviceCreateBuffer(g.ctx->Device(), &bd);

			WGPUTexelCopyTextureInfo srcInfo{};
			srcInfo.texture  = g.frame.surfaceTex;
			srcInfo.aspect   = WGPUTextureAspect_All;

			WGPUTexelCopyBufferInfo dstInfo{};
			dstInfo.buffer = staging;
			dstInfo.layout.bytesPerRow  = paddedBpr;
			dstInfo.layout.rowsPerImage = h;

			WGPUExtent3D ext{};
			ext.width = w; ext.height = h; ext.depthOrArrayLayers = 1;

			wgpuCommandEncoderCopyTextureToBuffer(g.frame.encoder, &srcInfo, &dstInfo, &ext);
			outBpr = paddedBpr;
			return staging;
		}

		void FinishScreenshot(WGPUBuffer staging, uint32_t paddedBpr,
		                      uint32_t w, uint32_t h, const std::string& path)
		{
			const uint64_t total = (uint64_t)paddedBpr * h;

			volatile bool done = false;
			WGPUBufferMapCallbackInfo cb{};
			cb.mode      = WGPUCallbackMode_AllowProcessEvents;
			cb.callback  = OnBufferMapped;
			cb.userdata1 = (void*)&done;
			(void)wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, total, cb);
			while (!done) wgpuInstanceProcessEvents(g.ctx->Instance());

			const uint8_t* mapped = (const uint8_t*)wgpuBufferGetConstMappedRange(staging, 0, total);
			std::vector<uint8_t> tight((size_t)w * h * 4);
			for (uint32_t y = 0; y < h; ++y) {
				std::memcpy(tight.data() + (size_t)y * w * 4,
				            mapped + (size_t)y * paddedBpr,
				            (size_t)w * 4);
			}
			SwizzleBGRAtoRGBA(tight.data(), (size_t)w * h);
			stbi_write_png(path.c_str(), (int)w, (int)h, 4, tight.data(), (int)(w * 4));
			INFO_CORE("Renderer: screenshot saved to {0}", path);

			wgpuBufferUnmap(staging);
			wgpuBufferRelease(staging);
		}

	} // namespace


	void Renderer::Init(WGPUContext* ctx)  { g.ctx = ctx; }
	void Renderer::Shutdown()              { g = State{}; }

	bool Renderer::BeginScene(const SPtr<Camera>& camera)
	{
		g.camera = camera;
		g.frame  = g.ctx->BeginFrame();
		return g.frame.valid;
	}

	void Renderer::OpenColorPass(float r, float gC, float b, float a,
	                             const WGPUPassTimestampWrites* timestampWrites)
	{
		if (!g.frame.valid || g.pass) return;
		g.pass = g.ctx->OpenColorPass(g.frame, r, gC, b, a, timestampWrites);
	}

	void Renderer::ClosePass()
	{
		if (g.pass) {
			g.ctx->ClosePass(g.pass);
			g.pass = nullptr;
		}
	}

	void Renderer::EndScene()
	{
		if (g.pass) {
			g.ctx->ClosePass(g.pass);
			g.pass = nullptr;
		}

		WGPUBuffer  staging   = nullptr;
		uint32_t    paddedBpr = 0;
		uint32_t    w = 0, h = 0;
		std::string outPath;
		const bool capturing = g.screenshotPending && g.frame.valid;
		if (capturing) {
			w = g.ctx->Width(); h = g.ctx->Height();
			outPath = g.screenshotPath;
			staging = EmitScreenshotCopy(paddedBpr);
		}

		g.ctx->EndFrame(g.frame);
		g.camera.reset();

		if (capturing) {
			FinishScreenshot(staging, paddedBpr, w, h, outPath);
			std::exit(0);
		}
	}

	void Renderer::RequestScreenshot(const std::string& pngPath)
	{
		g.screenshotPath    = pngPath;
		g.screenshotPending = true;
	}

	WGPUContext*          Renderer::Context()    { return g.ctx; }
	WGPUCommandEncoder    Renderer::Encoder()    { return g.frame.encoder; }
	WGPURenderPassEncoder Renderer::CurrentPass(){ return g.pass; }
	const SPtr<Camera>&   Renderer::GetCamera()  { return g.camera; }

}
