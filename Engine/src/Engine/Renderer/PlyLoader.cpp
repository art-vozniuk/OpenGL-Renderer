#include "pch.h"
#include "PlyLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

	namespace {

		// Constant for the SH0 basis function. 3DGS stores per-splat "f_dc"
		// as linear-space coefficients; the user-visible colour is
		//     c = 0.5 + SH_C0 * f_dc
		// (clamped to [0, 1]). Same value the reference renderer uses.
		constexpr float kShC0 = 0.28209479177387814f;

		// Number of view-dependent SH coefficients per channel for degree-3
		// (matches Inria's 3DGS default: (deg+1)² - 1 = 15).
		constexpr int kShRestCount = 15;

		// Total SH coefficients per channel including the DC term. We store
		// DC in the same texture as the rest (at slot 0) so the shader can
		// evaluate DC + bands 1..3 in one pass, matching the reference CUDA
		// rasterizer's `computeColorFromSH` and avoiding the precision loss
		// that uint8-baking the DC colour would introduce at highlights.
		constexpr int kShFullCount = 1 + kShRestCount;  // 16

		inline float Sigmoid(float x)
		{
			// Numerically stable: split on sign to avoid overflow in exp().
			if (x >= 0.0f) {
				const float e = std::exp(-x);
				return 1.0f / (1.0f + e);
			}
			const float e = std::exp(x);
			return e / (1.0f + e);
		}

		inline uint8_t FloatToByte(float v)
		{
			const float clamped = std::clamp(v, 0.0f, 1.0f);
			return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
		}

		// Strip trailing '\r' (PLYs from Windows tools often ship CRLF line
		// endings inside the ASCII header) and leading whitespace.
		void TrimLine(std::string& s)
		{
			while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
			size_t i = 0;
			while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
			if (i > 0) s.erase(0, i);
		}

		struct PlyProp {
			std::string name;    // "x", "f_rest_12", etc.
			size_t      offset;  // byte offset within a vertex record
			// Width in bytes — we only accept `float` (4B) for 3DGS outputs;
			// anything else is logged and the load bails out.
			size_t      width;
		};

		struct PlyHeader {
			size_t vertexCount   = 0;
			size_t recordStride  = 0;   // bytes per vertex
			size_t headerBytes   = 0;   // offset of the binary payload
			bool   littleEndian  = true;
			std::unordered_map<std::string, PlyProp> props;
		};

		// Reads header up to and including the `end_header\n` line. Leaves
		// the stream positioned at the start of the binary payload.
		bool ParseHeader(std::ifstream& in, PlyHeader& out, std::string& err)
		{
			std::string line;
			if (!std::getline(in, line)) { err = "empty file"; return false; }
			TrimLine(line);
			if (line != "ply") { err = "missing 'ply' magic"; return false; }

			bool sawFormat = false;
			size_t currentOffset = 0;
			// PLY can have multiple `element` blocks; 3DGS has only "vertex".
			// We track whether the last `element` seen was "vertex" and attach
			// any subsequent `property` lines to it.
			bool inVertex = false;

			while (std::getline(in, line)) {
				TrimLine(line);
				if (line.empty() || line.rfind("comment", 0) == 0) continue;

				if (line == "end_header") {
					// Stream position after end_header's newline is the start
					// of the payload. tellg() works because std::getline
					// consumed the delimiter.
					out.headerBytes = static_cast<size_t>(in.tellg());
					break;
				}

				std::istringstream ls(line);
				std::string tok;
				ls >> tok;

				if (tok == "format") {
					std::string kind; ls >> kind;
					if (kind == "binary_little_endian")      out.littleEndian = true;
					else if (kind == "binary_big_endian")    out.littleEndian = false;
					else { err = "unsupported format: " + kind; return false; }
					sawFormat = true;
				}
				else if (tok == "element") {
					std::string name; size_t n = 0;
					ls >> name >> n;
					if (name == "vertex") {
						out.vertexCount = n;
						inVertex = true;
					} else {
						inVertex = false;  // skip properties for non-vertex elements
					}
				}
				else if (tok == "property") {
					if (!inVertex) continue;
					std::string type; ls >> type;
					// Only support float (4B) — everything 3DGS writes.
					if (type != "float" && type != "float32") {
						err = "unsupported property type: " + type;
						return false;
					}
					std::string name; ls >> name;
					PlyProp p{ name, currentOffset, 4 };
					out.props.emplace(name, p);
					currentOffset += 4;
				}
			}

			if (!sawFormat || out.vertexCount == 0) {
				err = "malformed header (no format or zero vertices)";
				return false;
			}
			if (!out.littleEndian) {
				// We're running on little-endian hardware; big-endian PLYs
				// would need byte-swapping. Not supported until we meet one.
				err = "big-endian PLY not supported";
				return false;
			}
			out.recordStride = currentOffset;
			return true;
		}

		// Look up a property offset; returns SIZE_MAX when absent. Callers
		// decide whether missing a given field is fatal or optional.
		size_t FindOffset(const PlyHeader& h, const std::string& name)
		{
			const auto it = h.props.find(name);
			return (it == h.props.end()) ? SIZE_MAX : it->second.offset;
		}

		inline float ReadFloat(const uint8_t* base, size_t off)
		{
			float v;
			std::memcpy(&v, base + off, sizeof(float));
			return v;
		}

	}


	SplatData PlyLoader::LoadPly(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			ERROR_CORE("PlyLoader: cannot open '{0}'", path);
			return {};
		}

		PlyHeader header;
		std::string err;
		if (!ParseHeader(in, header, err)) {
			ERROR_CORE("PlyLoader: '{0}' — {1}", path, err);
			return {};
		}

		// Mandatory offsets. If any are missing the file isn't a 3DGS PLY.
		const size_t offX = FindOffset(header, "x");
		const size_t offY = FindOffset(header, "y");
		const size_t offZ = FindOffset(header, "z");
		const size_t offSx = FindOffset(header, "scale_0");
		const size_t offSy = FindOffset(header, "scale_1");
		const size_t offSz = FindOffset(header, "scale_2");
		const size_t offRw = FindOffset(header, "rot_0");
		const size_t offRx = FindOffset(header, "rot_1");
		const size_t offRy = FindOffset(header, "rot_2");
		const size_t offRz = FindOffset(header, "rot_3");
		const size_t offOp = FindOffset(header, "opacity");
		const size_t offDc0 = FindOffset(header, "f_dc_0");
		const size_t offDc1 = FindOffset(header, "f_dc_1");
		const size_t offDc2 = FindOffset(header, "f_dc_2");
		{
			const size_t mandatory[] = { offX, offY, offZ, offSx, offSy, offSz,
			                             offRw, offRx, offRy, offRz, offOp,
			                             offDc0, offDc1, offDc2 };
			for (size_t o : mandatory) {
				if (o == SIZE_MAX) {
					ERROR_CORE("PlyLoader: '{0}' — missing required 3DGS property", path);
					return {};
				}
			}
		}

		// Optional: SH band 1..3 (15 coefs × 3 channels, INRIA order is
		//   [R_sh1..R_sh15,  G_sh1..G_sh15,  B_sh1..B_sh15]).
		// We accept "all 45 present" or "none"; partial is rejected.
		size_t offRest[kShRestCount * 3];
		bool hasShRest = (FindOffset(header, "f_rest_0") != SIZE_MAX);
		if (hasShRest) {
			for (int i = 0; i < kShRestCount * 3; ++i) {
				const std::string n = "f_rest_" + std::to_string(i);
				const size_t o = FindOffset(header, n);
				if (o == SIZE_MAX) {
					hasShRest = false;
					break;
				}
				offRest[i] = o;
			}
			if (!hasShRest) {
				WARN_CORE("PlyLoader: '{0}' has partial f_rest set — disabling SH",
				          path);
			}
		}

		// Read the binary payload in one go. Even for large files (~300 MB)
		// this is far faster than looping read(record) — one syscall vs N.
		const size_t payloadBytes = header.vertexCount * header.recordStride;
		std::vector<uint8_t> raw(payloadBytes);
		in.read(reinterpret_cast<char*>(raw.data()),
		        static_cast<std::streamsize>(payloadBytes));
		if (!in) {
			ERROR_CORE("PlyLoader: short read on '{0}' ({1} bytes)",
			           path, (uint64_t)payloadBytes);
			return {};
		}

		SplatData out;
		const size_t N = header.vertexCount;
		out.positions.resize(N);
		out.scales.resize(N);
		out.rotations.resize(N);
		out.colors.resize(N);
		if (hasShRest) {
			out.shCoefCount = kShFullCount;
			// Layout in shRest: per-coef RGB interleaved.
			//   slot 0           → DC (f_dc_0..2)
			//   slots 1..15      → bands 1..3 (f_rest_*)
			// Each splat occupies kShFullCount * 3 floats contiguously.
			out.shRest.resize(N * kShFullCount * 3);
		}

		for (size_t i = 0; i < N; ++i) {
			const uint8_t* rec = raw.data() + i * header.recordStride;

			// Position — Y-up flip (see SplatLoader.cpp for the same bake).
			const float px = ReadFloat(rec, offX);
			const float py = ReadFloat(rec, offY);
			const float pz = ReadFloat(rec, offZ);
			out.positions[i] = glm::vec3(px, -py, -pz);

			// Scale — PLY stores log-space, exp() gives world-space sigma.
			out.scales[i] = glm::vec3(
				std::exp(ReadFloat(rec, offSx)),
				std::exp(ReadFloat(rec, offSy)),
				std::exp(ReadFloat(rec, offSz)));

			// Rotation quaternion (w, x, y, z). Re-normalize (training output
			// is close to unit but not guaranteed), then bake the Y-up X180°
			// rotation the same way SplatLoader does.
			float qw = ReadFloat(rec, offRw);
			float qx = ReadFloat(rec, offRx);
			float qy = ReadFloat(rec, offRy);
			float qz = ReadFloat(rec, offRz);
			const float qlen = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
			if (qlen > 0.0f) {
				const float inv = 1.0f / qlen;
				qw *= inv; qx *= inv; qy *= inv; qz *= inv;
			} else {
				qw = 1.0f; qx = qy = qz = 0.0f;
			}
			// Quaternion Y-up bake: conjugate by qX180° → negate y, z comps.
			out.rotations[i] = glm::vec4(qw, qx, -qy, -qz);

			// DC coefficients (SH band 0) + opacity. When SH is on we also
			// store the DC in the texture (uncorked) so the shader can do
			// the exact reference eval; the baked uint8 colour stays as a
			// fallback for the flat-colour shader path.
			const float dc0 = ReadFloat(rec, offDc0);
			const float dc1 = ReadFloat(rec, offDc1);
			const float dc2 = ReadFloat(rec, offDc2);
			const float alpha = Sigmoid(ReadFloat(rec, offOp));
			out.colors[i] = glm::u8vec4(
				FloatToByte(0.5f + kShC0 * dc0),
				FloatToByte(0.5f + kShC0 * dc1),
				FloatToByte(0.5f + kShC0 * dc2),
				FloatToByte(alpha));

			// Pack DC + SH bands 1..3 into the single SH texture. Re-interleave
			// f_rest from R-all,G-all,B-all into per-coef RGB for one texelFetch
			// per coef on the GPU side.
			if (hasShRest) {
				float* dst = out.shRest.data() + i * kShFullCount * 3;
				// Slot 0 — raw DC (shader applies SH_C0 itself to keep the
				// algebra identical to reference CUDA rasterizer).
				dst[0 * 3 + 0] = dc0;
				dst[0 * 3 + 1] = dc1;
				dst[0 * 3 + 2] = dc2;
				// Slots 1..15 — bands 1..3.
				for (int c = 0; c < kShRestCount; ++c) {
					dst[(1 + c) * 3 + 0] = ReadFloat(rec, offRest[c]);
					dst[(1 + c) * 3 + 1] = ReadFloat(rec, offRest[kShRestCount + c]);
					dst[(1 + c) * 3 + 2] = ReadFloat(rec, offRest[2 * kShRestCount + c]);
				}
			}
		}

		INFO_CORE("PlyLoader: loaded {0} splats from '{1}' (SH={2})",
		          (uint64_t)N, path, hasShRest ? "on" : "off");
		return out;
	}

}
