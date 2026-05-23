#include "pch.h"
#include "GltfLoader.h"

#include "Engine/Log.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"

#include "stb_image.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Engine {

	namespace {

		void ReadVec2(const cgltf_accessor* acc, std::vector<glm::vec2>& out)
		{
			if (!acc) return;
			out.resize(acc->count);
			for (size_t i = 0; i < acc->count; ++i) {
				float tmp[2] = {0,0};
				cgltf_accessor_read_float(acc, i, tmp, 2);
				out[i] = glm::vec2(tmp[0], tmp[1]);
			}
		}
		void ReadVec3(const cgltf_accessor* acc, std::vector<glm::vec3>& out)
		{
			if (!acc) return;
			out.resize(acc->count);
			for (size_t i = 0; i < acc->count; ++i) {
				float tmp[3] = {0,0,0};
				cgltf_accessor_read_float(acc, i, tmp, 3);
				out[i] = glm::vec3(tmp[0], tmp[1], tmp[2]);
			}
		}
		void ReadVec4(const cgltf_accessor* acc, std::vector<glm::vec4>& out)
		{
			if (!acc) return;
			out.resize(acc->count);
			for (size_t i = 0; i < acc->count; ++i) {
				float tmp[4] = {0,0,0,0};
				cgltf_accessor_read_float(acc, i, tmp, 4);
				out[i] = glm::vec4(tmp[0], tmp[1], tmp[2], tmp[3]);
			}
		}

		void ReadIndices(const cgltf_accessor* acc, std::vector<uint32_t>& out)
		{
			if (!acc) return;
			out.resize(acc->count);
			for (size_t i = 0; i < acc->count; ++i) {
				out[i] = static_cast<uint32_t>(cgltf_accessor_read_index(acc, i));
			}
		}

		// Decode embedded image bytes (from a buffer view) into RGBA8 pixels.
		bool DecodeImage(const uint8_t* bytes, size_t size,
		                 std::vector<uint8_t>& outPixels, int& outW, int& outH)
		{
			int w = 0, h = 0, c = 0;
			stbi_uc* px = stbi_load_from_memory(bytes, (int)size, &w, &h, &c, 4);
			if (!px) {
				WARN_CORE("gltf: stbi_load_from_memory failed: {0}", stbi_failure_reason());
				return false;
			}
			outPixels.assign(px, px + (size_t)w * h * 4);
			outW = w; outH = h;
			stbi_image_free(px);
			return true;
		}

		// Pull the raw bytes for a glTF image — either inline (buffer view)
		// or, for .glb, embedded in the binary chunk.
		bool ExtractImageBytes(const cgltf_image* img,
		                       const uint8_t** outData, size_t* outSize)
		{
			if (!img || !img->buffer_view) return false;
			const cgltf_buffer_view* bv = img->buffer_view;
			if (!bv->buffer || !bv->buffer->data) return false;
			*outData = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
			*outSize = bv->size;
			return true;
		}

		void LoadTextureInto(const cgltf_texture_view& tv,
		                     std::vector<uint8_t>& outPx, int& outW, int& outH)
		{
			if (!tv.texture || !tv.texture->image) return;
			const uint8_t* data = nullptr; size_t size = 0;
			if (!ExtractImageBytes(tv.texture->image, &data, &size)) return;
			DecodeImage(data, size, outPx, outW, outH);
		}

		// Cross-product tangent approximation when the file doesn't ship
		// tangents. Picks a stable direction perpendicular to the normal —
		// not artistically correct but enough to make textured normal maps
		// look acceptable.
		void GenerateTangents(MeshPrimitive& p)
		{
			p.tangents.assign(p.normals.size(), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
			for (size_t i = 0; i < p.normals.size(); ++i) {
				const glm::vec3 n = glm::normalize(p.normals[i]);
				glm::vec3 t;
				if (std::abs(n.y) < 0.9f) t = glm::normalize(glm::cross(n, glm::vec3(0,1,0)));
				else                      t = glm::normalize(glm::cross(n, glm::vec3(1,0,0)));
				p.tangents[i] = glm::vec4(t, 1.0f);
			}
		}

	} // namespace


	MeshData GltfLoader::LoadGlbFromBytes(const uint8_t* data, size_t size, const char* label)
	{
		MeshData out;
		if (!data || size < 12) {
			ERROR_CORE("gltf: payload too small ({0} bytes) from {1}", size, label);
			return out;
		}

		cgltf_options opts{};
		cgltf_data* gltf = nullptr;
		cgltf_result r = cgltf_parse(&opts, data, size, &gltf);
		if (r != cgltf_result_success) {
			ERROR_CORE("gltf: cgltf_parse failed (code {0}) for {1}", (int)r, label);
			return out;
		}
		// load_buffers picks up the embedded .glb BIN chunk.
		r = cgltf_load_buffers(&opts, gltf, nullptr);
		if (r != cgltf_result_success) {
			ERROR_CORE("gltf: cgltf_load_buffers failed (code {0}) for {1}", (int)r, label);
			cgltf_free(gltf);
			return out;
		}

		// Materials.
		out.materials.resize(gltf->materials_count);
		for (size_t i = 0; i < gltf->materials_count; ++i) {
			const cgltf_material& src = gltf->materials[i];
			MeshMaterial& dst = out.materials[i];

			if (src.has_pbr_metallic_roughness) {
				const auto& pbr = src.pbr_metallic_roughness;
				dst.baseColorFactor = glm::vec4(
					pbr.base_color_factor[0], pbr.base_color_factor[1],
					pbr.base_color_factor[2], pbr.base_color_factor[3]);
				dst.metallicFactor  = pbr.metallic_factor;
				dst.roughnessFactor = pbr.roughness_factor;
				LoadTextureInto(pbr.base_color_texture,
				                dst.baseColorPixels, dst.baseColorWidth, dst.baseColorHeight);
				LoadTextureInto(pbr.metallic_roughness_texture,
				                dst.mrPixels, dst.mrWidth, dst.mrHeight);
			}
			LoadTextureInto(src.normal_texture,
			                dst.normalPixels, dst.normalWidth, dst.normalHeight);
		}

		// Primitives across all meshes/nodes. We don't honour the node
		// hierarchy in v1 — meshes are flattened and the editor applies
		// the per-object transform on top.
		glm::vec3 mn(std::numeric_limits<float>::max());
		glm::vec3 mx(-std::numeric_limits<float>::max());
		bool anyAabb = false;

		for (size_t mi = 0; mi < gltf->meshes_count; ++mi) {
			const cgltf_mesh& mesh = gltf->meshes[mi];
			for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
				const cgltf_primitive& prim = mesh.primitives[pi];
				if (prim.type != cgltf_primitive_type_triangles) continue;

				MeshPrimitive dst;
				for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
					const cgltf_attribute& a = prim.attributes[ai];
					switch (a.type) {
						case cgltf_attribute_type_position: ReadVec3(a.data, dst.positions); break;
						case cgltf_attribute_type_normal:   ReadVec3(a.data, dst.normals);   break;
						case cgltf_attribute_type_texcoord:
							if (a.index == 0) ReadVec2(a.data, dst.uvs);
							break;
						case cgltf_attribute_type_tangent:  ReadVec4(a.data, dst.tangents);  break;
						default: break;
					}
				}
				ReadIndices(prim.indices, dst.indices);
				if (dst.positions.empty()) continue;

				// Fill defaults so the vertex layout is always populated.
				if (dst.normals.empty()) dst.normals.assign(dst.positions.size(), glm::vec3(0,1,0));
				if (dst.uvs.empty())     dst.uvs.assign(dst.positions.size(),     glm::vec2(0.0f));
				if (dst.tangents.empty()) GenerateTangents(dst);

				// AABB.
				for (const auto& p : dst.positions) {
					mn = glm::min(mn, p);
					mx = glm::max(mx, p);
				}
				anyAabb = true;

				// Material index.
				if (prim.material) {
					dst.materialIndex = (int)(prim.material - gltf->materials);
				}

				out.primitives.push_back(std::move(dst));
			}
		}

		if (anyAabb) {
			out.aabbMin = mn;
			out.aabbMax = mx;
			out.aabbValid = true;
		}

		INFO_CORE("gltf: parsed '{0}': {1} primitives, {2} materials",
		          label, (uint64_t)out.primitives.size(), (uint64_t)out.materials.size());

		cgltf_free(gltf);
		return out;
	}

}
