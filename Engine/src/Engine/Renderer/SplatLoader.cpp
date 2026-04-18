#include "pch.h"
#include "SplatLoader.h"

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

		std::ifstream in(path, std::ios::binary);
		if (!in) {
			ERROR_CORE("SplatLoader: cannot open '{0}'", path);
			return {};
		}

		const size_t count = static_cast<size_t>(size) / sizeof(PackedSplat);
		std::vector<PackedSplat> raw(count);
		in.read(reinterpret_cast<char*>(raw.data()),
		        static_cast<std::streamsize>(size));
		if (!in) {
			ERROR_CORE("SplatLoader: short read on '{0}'", path);
			return {};
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

		for (size_t i = 0; i < count; ++i) {
			const PackedSplat& p = raw[i];

			out.positions[i] = glm::vec3(p.pos[0], p.pos[1], p.pos[2]);
			out.scales[i]    = glm::vec3(p.scale[0], p.scale[1], p.scale[2]);

			glm::vec4 q(
				(static_cast<float>(p.rotation[0]) - 128.0f) * kQuatScale,  // w
				(static_cast<float>(p.rotation[1]) - 128.0f) * kQuatScale,  // x
				(static_cast<float>(p.rotation[2]) - 128.0f) * kQuatScale,  // y
				(static_cast<float>(p.rotation[3]) - 128.0f) * kQuatScale); // z
			const float len = glm::length(q);
			out.rotations[i] = (len > 0.0f) ? q / len : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);

			out.colors[i] = glm::u8vec4(p.color[0], p.color[1], p.color[2], p.color[3]);
		}

		INFO_CORE("SplatLoader: loaded {0} splats from '{1}'", (uint64_t)count, path);
		return out;
	}

}
