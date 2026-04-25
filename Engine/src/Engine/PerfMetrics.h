#pragma once

#include <algorithm>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Engine {

	/*
	 * Rolling 5-second window of per-frame samples. Push() each frame,
	 * Current/Avg/Max snapshot the window for the perf overlay.
	 *
	 * Ring buffer is fixed-size, no allocation. At 60fps the 300-slot
	 * buffer covers exactly 5 seconds; below 60fps the window is longer
	 * (still bounded by capacity).
	 */
	struct PerfSamples
	{
		static constexpr int kCapacity = 300;

		float buf[kCapacity] = {};
		int   head  = 0;
		int   count = 0;

		void Push(float v)
		{
			buf[head] = v;
			head      = (head + 1) % kCapacity;
			if (count < kCapacity) count++;
		}

		float Current() const
		{
			if (count == 0) return 0.0f;
			const int i = (head + kCapacity - 1) % kCapacity;
			return buf[i];
		}

		float Avg() const
		{
			if (count == 0) return 0.0f;
			float sum = 0.0f;
			for (int i = 0; i < count; ++i) sum += buf[i];
			return sum / static_cast<float>(count);
		}

		float Max() const
		{
			if (count == 0) return 0.0f;
			float m = buf[0];
			for (int i = 1; i < count; ++i) if (buf[i] > m) m = buf[i];
			return m;
		}
	};


	/*
	 * Per-frame perf metrics for the Gaussian-splat renderer overlay.
	 * Owned by the scene; populated by the renderer (CPU encode time
	 * always, GPU times only when timestamp-query was granted).
	 *
	 * Emit() serialises a small JSON blob to the parent frame via
	 * postMessage. The React side rate-limits display so calling this
	 * every frame is fine.
	 */
	struct PerfMetrics
	{
		PerfSamples frameMs;       // wall-clock between frame begins
		PerfSamples cpuEncodeMs;   // time spent building the command encoder
		PerfSamples gpuSortMs;     // GPU time inside the sort dispatches
		PerfSamples gpuRenderMs;   // GPU time inside the render pass
		PerfSamples gpuTotalMs;    // sort + render

		int  splatCount     = 0;
		bool gpuTimingsValid = false;  // false → only CPU samples are meaningful

		void Emit() const
		{
		#ifdef __EMSCRIPTEN__
			char buf[640];
			std::snprintf(
				buf, sizeof(buf),
				"{\"type\":\"perf\","
				"\"frame\":{\"cur\":%.2f,\"avg\":%.2f,\"max\":%.2f},"
				"\"cpuEncode\":{\"cur\":%.2f,\"avg\":%.2f,\"max\":%.2f},"
				"\"gpuSort\":{\"cur\":%.2f,\"avg\":%.2f,\"max\":%.2f},"
				"\"gpuRender\":{\"cur\":%.2f,\"avg\":%.2f,\"max\":%.2f},"
				"\"gpuTotal\":{\"cur\":%.2f,\"avg\":%.2f,\"max\":%.2f},"
				"\"splats\":%d,\"gpuValid\":%s}",
				frameMs.Current(),     frameMs.Avg(),     frameMs.Max(),
				cpuEncodeMs.Current(), cpuEncodeMs.Avg(), cpuEncodeMs.Max(),
				gpuSortMs.Current(),   gpuSortMs.Avg(),   gpuSortMs.Max(),
				gpuRenderMs.Current(), gpuRenderMs.Avg(), gpuRenderMs.Max(),
				gpuTotalMs.Current(),  gpuTotalMs.Avg(),  gpuTotalMs.Max(),
				splatCount,
				gpuTimingsValid ? "true" : "false");

			EM_ASM({
				try {
					if (typeof window !== 'undefined' && window.parent !== window) {
						window.parent.postMessage(JSON.parse(UTF8ToString($0)), '*');
					}
				} catch (e) {}
			}, buf);
		#endif
		}
	};

}
