#include "pch.h"
#include "SplatLoader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace Engine {

	namespace {

		// Packed layout of a single splat record as it appears on disk.
		// Keeping this as a POD with identical byte offsets to the file lets
		// us bulk-read into a vector<> and then copy columns out, instead of
		// hand-parsing one field at a time with unaligned reads.
		#pragma pack(push, 1)
		struct PackedSplat
		{
			float   pos[3];
			float   scale[3];
			uint8_t color[4];
			uint8_t rotation[4];
		};
		#pragma pack(pop)
		static_assert(sizeof(PackedSplat) == 32, "PackedSplat must be 32 bytes");

	}


	SplatData SplatLoader::LoadSplat(const std::string& path)
	{
		namespace fs = std::filesystem;

		std::error_code ec;
		const auto size = fs::file_size(path, ec);
		if (ec) {
			ERROR_CORE("SplatLoader: cannot stat '{0}': {1}", path, ec.message());
			return {};
		}

		if (size == 0 || size % sizeof(PackedSplat) != 0) {
			ERROR_CORE("SplatLoader: '{0}' is {1} bytes — not a multiple of {2}",
			           path, (uint64_t)size, (int)sizeof(PackedSplat));
			return {};
		}

		// Use C stdio with explicit "rb" — on emscripten's MEMFS (preloaded
		// from the .data bundle), std::ifstream with std::ios::binary has
		// been observed to hand back corrupted bytes on some Android WebKit
		// browsers (bbox came back ~10^30 instead of the real ~±30 range).
		// fopen/fread go through the same FS layer but with a simpler path
		// that has been reliable in the field.
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f) {
			ERROR_CORE("SplatLoader: cannot open '{0}'", path);
			return {};
		}

		const size_t count = static_cast<size_t>(size) / sizeof(PackedSplat);
		std::vector<PackedSplat> raw(count);
		const size_t got = std::fread(raw.data(), sizeof(PackedSplat), count, f);
		std::fclose(f);
		if (got != count) {
			ERROR_CORE("SplatLoader: short read on '{0}' ({1}/{2} records)",
			           path, (uint64_t)got, (uint64_t)count);
			return {};
		}

		// Sanity log: first record's position bytes. When the deployed web
		// bundle silently corrupts the .data file, this line surfaces
		// the issue in the browser console ("pos0 = 1e+30, ..." vs the
		// expected small world-space float) before any rendering happens.
		{
			const PackedSplat& s0 = raw.front();
			INFO_CORE("SplatLoader: splat0.pos=({0},{1},{2}) scale=({3},{4},{5})",
			          s0.pos[0], s0.pos[1], s0.pos[2],
			          s0.scale[0], s0.scale[1], s0.scale[2]);
		}

		SplatData out;
		out.positions.resize(count);
		out.scales.resize(count);
		out.rotations.resize(count);
		out.colors.resize(count);

		// Constant for quaternion byte → float decode: (byte - 128) / 128
		// maps [0, 255] into roughly [-1, 1]. Result is unit-length by
		// construction (the baker enforced that during quantization), but
		// we re-normalize to defend against accumulated error.
		constexpr float kQuatScale = 1.0f / 128.0f;

		// Gaussian-splat captures use the COLMAP / OpenCV camera frame where
		// +Y is down in the world. We bake a 180°-rotation-around-X transform
		// into the loaded data so the rest of the engine can treat the splats
		// as a normal +Y-up scene:
		//   position  (x, y, z)      →  (x, -y, -z)
		//   rotation  (w, x, y, z)   →  (w, x, -y, -z)   [quaternion conjugation by qX180]
		// Applying the transform once here is cheaper than re-deriving it
		// in every sort / camera-setup call downstream.

		for (size_t i = 0; i < count; ++i) {
			const PackedSplat& p = raw[i];

			out.positions[i] = glm::vec3(p.pos[0], -p.pos[1], -p.pos[2]);
			out.scales[i]    = glm::vec3(p.scale[0], p.scale[1], p.scale[2]);

			// vec4 layout here is (w, x, y, z).
			glm::vec4 q(
				(static_cast<float>(p.rotation[0]) - 128.0f) * kQuatScale,  // w
				(static_cast<float>(p.rotation[1]) - 128.0f) * kQuatScale,  // x
				(static_cast<float>(p.rotation[2]) - 128.0f) * kQuatScale,  // y
				(static_cast<float>(p.rotation[3]) - 128.0f) * kQuatScale); // z
			// Apply the same X-axis 180° rotation to the splat's orientation.
			q.z = -q.z;  // negate y component
			q.w = -q.w;  // negate z component
			const float len = glm::length(q);
			out.rotations[i] = (len > 0.0f) ? q / len : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);

			out.colors[i] = glm::u8vec4(p.color[0], p.color[1], p.color[2], p.color[3]);
		}

		INFO_CORE("SplatLoader: loaded {0} splats from '{1}'", (uint64_t)count, path);
		return out;
	}

}
