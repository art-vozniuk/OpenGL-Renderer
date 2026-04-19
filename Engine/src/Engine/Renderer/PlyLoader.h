#pragma once

#include "SplatLoader.h"

#include <string>

namespace Engine {

	/*
	 * PLY loader for the Inria 3D-Gaussian-Splatting output format.
	 *
	 * The .ply file is a standard "ply / binary_little_endian 1.0" header
	 * followed by one binary record per vertex. The 3DGS training code writes
	 * these properties, in this exact order, as float32:
	 *
	 *   x, y, z,  nx, ny, nz,
	 *   f_dc_0..2,                 // SH band 0 (DC), 1 coef × 3 channels
	 *   f_rest_0..44,              // SH bands 1..3, 15 coefs × 3 channels
	 *   opacity,                   // logit-space; needs sigmoid()
	 *   scale_0..2,                // log-space; needs exp()
	 *   rot_0..3                   // quaternion (w, x, y, z); re-normalized
	 *
	 * f_rest packs channels outer, coefs inner:
	 *   [R_sh1..R_sh15,  G_sh1..G_sh15,  B_sh1..B_sh15]
	 * The loader re-interleaves this into RGB-per-coef order in SplatData.shRest
	 * so the GPU shader can do a single RGB texelFetch per coef instead of three.
	 *
	 * The resulting SplatData is directly compatible with the existing
	 * antimatter15 .splat path — `positions/scales/rotations/colors` are
	 * populated the same way (Y-up, exp-scale, sigmoid-opacity). If the file
	 * is older and lacks SH coefs, shRest is left empty.
	 */
	class PlyLoader
	{
	public:
		// Parses a 3DGS PLY file from disk. Returns an empty SplatData on
		// failure (unknown header, truncated file, unsupported property types).
		static SplatData LoadPly(const std::string& path);
	};

}
