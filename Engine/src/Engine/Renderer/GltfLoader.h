#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Engine {

	// One material — uniforms + (optional) RGBA8 texture pixels.
	// Textures are decoded to RGBA8 by stb_image when present.
	struct MeshMaterial
	{
		glm::vec4 baseColorFactor = glm::vec4(1.0f);
		float     metallicFactor  = 1.0f;
		float     roughnessFactor = 1.0f;

		std::vector<uint8_t> baseColorPixels;
		int baseColorWidth  = 0;
		int baseColorHeight = 0;

		std::vector<uint8_t> normalPixels;
		int normalWidth  = 0;
		int normalHeight = 0;

		std::vector<uint8_t> mrPixels;
		int mrWidth  = 0;
		int mrHeight = 0;
	};

	// One primitive (= one drawable).
	struct MeshPrimitive
	{
		std::vector<glm::vec3> positions;
		std::vector<glm::vec3> normals;
		std::vector<glm::vec2> uvs;
		std::vector<glm::vec4> tangents;   // xyz = tangent, w = handedness
		std::vector<uint32_t>  indices;
		int materialIndex = -1;
	};

	// Whole-file payload — N primitives + M materials.
	struct MeshData
	{
		std::vector<MeshPrimitive> primitives;
		std::vector<MeshMaterial>  materials;

		// AABB across all primitives (object-space).
		glm::vec3 aabbMin = glm::vec3(0.0f);
		glm::vec3 aabbMax = glm::vec3(0.0f);
		bool      aabbValid = false;

		bool Empty() const { return primitives.empty(); }
	};


	class GltfLoader
	{
	public:
		// Parses a .glb blob. On failure returns an empty MeshData and logs.
		static MeshData LoadGlbFromBytes(const uint8_t* data, size_t size,
		                                 const char* sourceLabel = "memory");
	};

}
