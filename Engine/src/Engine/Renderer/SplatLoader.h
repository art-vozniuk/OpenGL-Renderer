#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	/*
	 * Gaussian splat loader for the antimatter15 ".splat" binary format.
	 * Each record is 32 bytes, no header, tightly packed:
	 *   float32 position[3]   // bytes  0..11
	 *   float32 scale[3]      // bytes 12..23  (real scale, not log-space)
	 *   uint8   color[4]      // bytes 24..27  (r, g, b, a  in 0..255)
	 *   uint8   rotation[4]   // bytes 28..31  (w, x, y, z  as (v - 128) / 128)
	 *
	 * Parsed result is Structure-of-Arrays — the CPU-side sorter and the
	 * per-instance GPU upload both want contiguous columns rather than an
	 * array of structs.
	 */
	struct SplatData
	{
		std::vector<glm::vec3> positions;  // world-space centroid
		std::vector<glm::vec3> scales;     // per-axis Gaussian sigma
		std::vector<glm::vec4> rotations;  // quaternion (w, x, y, z), unit-length
		std::vector<glm::u8vec4> colors;   // premultiplied RGBA in 0..255

		// Optional view-dependent spherical-harmonic coefficients (bands 1..3,
		// 15 coefs × 3 channels = 45 floats per splat). Empty when the source
		// file had none (antimatter15 .splat) — in that case the renderer falls
		// back to the flat-color path. Stored in RGB-interleaved per-coef
		// order:  coef0.r, coef0.g, coef0.b, coef1.r, ..., coef14.b.
		// Band 0 (DC) is already folded into `colors` via the SH0 basis.
		std::vector<float> shRest;

		// Number of SH coefs per channel in `shRest` (15 for degree 3, 0 when
		// the dataset has no view-dependent term).
		int shCoefCount = 0;

		size_t Count() const { return positions.size(); }
		bool   Empty() const { return positions.empty(); }
		bool   HasSH() const { return shCoefCount > 0 && !shRest.empty(); }
	};


	class SplatLoader
	{
	public:
		// Parses a .splat file from disk. Returns an empty SplatData on failure
		// (the caller is expected to log the error and fall back gracefully).
		static SplatData LoadSplat(const std::string& path);
	};

}
