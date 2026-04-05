#ifdef __EMSCRIPTEN__

#include "pch.h"
#include "GltfLoader.h"
#include "Assets.h"

// Pull in the tinygltf single-header implementation here (one translation unit only).
// We skip stb_image because the Engine already has its own copy.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

glm::mat4 NodeLocalTransform(const tinygltf::Node& node)
{
	if (!node.matrix.empty()) {
		glm::mat4 m;
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				m[col][row] = static_cast<float>(node.matrix[col * 4 + row]);
		return m;
	}

	glm::mat4 t(1.f), r(1.f), s(1.f);

	if (node.translation.size() == 3)
		t = glm::translate(glm::mat4(1.f), glm::vec3(
			static_cast<float>(node.translation[0]),
			static_cast<float>(node.translation[1]),
			static_cast<float>(node.translation[2])));

	if (node.rotation.size() == 4) {
		glm::quat q(
			static_cast<float>(node.rotation[3]), // w
			static_cast<float>(node.rotation[0]),
			static_cast<float>(node.rotation[1]),
			static_cast<float>(node.rotation[2]));
		r = glm::mat4_cast(q);
	}

	if (node.scale.size() == 3)
		s = glm::scale(glm::mat4(1.f), glm::vec3(
			static_cast<float>(node.scale[0]),
			static_cast<float>(node.scale[1]),
			static_cast<float>(node.scale[2])));

	return t * r * s;
}

// Raw accessor read — returns a pointer to the first element of accessor[index].
template<typename T>
const T* AccessorPtr(const tinygltf::Model& gltf, int accessorIdx, size_t elementIdx = 0)
{
	const auto& acc  = gltf.accessors[accessorIdx];
	const auto& view = gltf.bufferViews[acc.bufferView];
	const auto& buf  = gltf.buffers[view.buffer];
	const size_t stride = view.byteStride ? view.byteStride : sizeof(T);
	const size_t offset = acc.byteOffset + view.byteOffset + elementIdx * stride;
	return reinterpret_cast<const T*>(buf.data.data() + offset);
}

// ---------------------------------------------------------------------------
// Build one Scn::Mesh from a glTF primitive
// ---------------------------------------------------------------------------

SPtr<Scn::Mesh> BuildMesh(const tinygltf::Model& gltf,
                           const tinygltf::Primitive& prim,
                           const glm::mat4& transform,
                           const SPtr<Scn::Material>& material)
{
	auto findAttr = [&](const char* name) -> int {
		auto it = prim.attributes.find(name);
		return (it != prim.attributes.end()) ? it->second : -1;
	};

	const int posIdx  = findAttr("POSITION");
	const int normIdx = findAttr("NORMAL");
	const int tanIdx  = findAttr("TANGENT");
	const int uvIdx   = findAttr("TEXCOORD_0");

	if (posIdx < 0) return nullptr; // no geometry

	const size_t vertCount = gltf.accessors[posIdx].count;
	std::vector<Scn::Vertex> verts;
	verts.reserve(vertCount);

	for (size_t i = 0; i < vertCount; ++i) {
		glm::vec3 pos(0.f), norm(0.f), tan(0.f), bitan(0.f);
		glm::vec2 uv(0.f);

		if (posIdx >= 0) {
			const float* p = AccessorPtr<float>(gltf, posIdx, i);
			pos = { p[0], p[1], p[2] };
		}
		if (normIdx >= 0) {
			const float* n = AccessorPtr<float>(gltf, normIdx, i);
			norm = { n[0], n[1], n[2] };
		}
		if (tanIdx >= 0) {
			// glTF tangent is vec4 (xyz = tangent, w = handedness)
			const float* t = AccessorPtr<float>(gltf, tanIdx, i);
			tan = { t[0], t[1], t[2] };
			// bitangent = cross(normal, tangent) * t[3]
			bitan = glm::cross(norm, tan) * t[3];
		}
		if (uvIdx >= 0) {
			const float* tc = AccessorPtr<float>(gltf, uvIdx, i);
			uv = { tc[0], tc[1] };
		}

		verts.emplace_back(pos, norm, tan, bitan, uv);
	}

	// Indices
	std::vector<Scn::Face> faces;
	if (prim.indices >= 0) {
		const auto& idxAcc = gltf.accessors[prim.indices];
		const size_t idxCount = idxAcc.count;
		faces.reserve(idxCount / 3);

		for (size_t i = 0; i + 2 < idxCount; i += 3) {
			uint i0, i1, i2;
			switch (idxAcc.componentType) {
			case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
				i0 = *AccessorPtr<uint16_t>(gltf, prim.indices, i + 0);
				i1 = *AccessorPtr<uint16_t>(gltf, prim.indices, i + 1);
				i2 = *AccessorPtr<uint16_t>(gltf, prim.indices, i + 2);
			} break;
			case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
				i0 = *AccessorPtr<uint32_t>(gltf, prim.indices, i + 0);
				i1 = *AccessorPtr<uint32_t>(gltf, prim.indices, i + 1);
				i2 = *AccessorPtr<uint32_t>(gltf, prim.indices, i + 2);
			} break;
			case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
				i0 = *AccessorPtr<uint8_t>(gltf, prim.indices, i + 0);
				i1 = *AccessorPtr<uint8_t>(gltf, prim.indices, i + 1);
				i2 = *AccessorPtr<uint8_t>(gltf, prim.indices, i + 2);
			} break;
			default:
				WARN_CORE("GltfLoader: unsupported index component type");
				continue;
			}
			faces.emplace_back(i0, i1, i2);
		}
	}

	return MakeShared<Scn::Mesh>(std::move(verts), std::move(faces), transform, material);
}

// ---------------------------------------------------------------------------
// Build a Scn::Material from a glTF material index
// ---------------------------------------------------------------------------

SPtr<Scn::Material> BuildMaterial(const tinygltf::Model& gltf, int matIdx,
                                   const std::string& baseDir)
{
	std::string matName = "material_" + std::to_string(matIdx);
	if (matIdx >= 0 && matIdx < static_cast<int>(gltf.materials.size())) {
		const auto& m = gltf.materials[matIdx];
		if (!m.name.empty())
			matName = m.name;
	}
	auto mat = MakeShared<Scn::Material>(matName, "default");

	if (matIdx < 0 || matIdx >= static_cast<int>(gltf.materials.size()))
		return mat;

	const auto& gMat = gltf.materials[matIdx];

	auto loadTex = [&](int texIdx, Scn::Texture::Type type) {
		if (texIdx < 0) return;
		const int imgIdx = gltf.textures[texIdx].source;
		if (imgIdx < 0) return;
		// URI is relative to the glTF file — prepend base directory
		const std::string fullPath = baseDir + gltf.images[imgIdx].uri;
		auto renderTex = AssetManager::GetTexture2D(fullPath);
		auto scnTex = MakeShared<Scn::Texture>(renderTex);
		scnTex->SetType(type);
		mat->AddTexture(scnTex);
	};

	// PBR base color texture → diffuse slot
	loadTex(gMat.pbrMetallicRoughness.baseColorTexture.index, Scn::Texture::Type::Diffuse);
	// Normal map
	loadTex(gMat.normalTexture.index, Scn::Texture::Type::Normal);

	return mat;
}

// ---------------------------------------------------------------------------
// Recursively walk glTF nodes
// ---------------------------------------------------------------------------

void ProcessNode(const tinygltf::Model& gltf,
                 int nodeIdx,
                 const glm::mat4& parentTransform,
                 Scn::Model& outModel,
                 std::vector<SPtr<Scn::Material>>& materials)
{
	const auto& node = gltf.nodes[nodeIdx];
	const glm::mat4 transform = parentTransform * NodeLocalTransform(node);

	if (node.mesh >= 0) {
		const auto& gMesh = gltf.meshes[node.mesh];
		for (const auto& prim : gMesh.primitives) {
			auto& mat = materials[prim.material >= 0 ? prim.material : 0];
			auto mesh = BuildMesh(gltf, prim, transform, mat);
			if (mesh)
				outModel.AddMesh(mesh);
		}
	}

	for (int child : node.children)
		ProcessNode(gltf, child, transform, outModel, materials);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

SPtr<Scn::Model> GltfLoader::Load(const std::string& path)
{
	tinygltf::TinyGLTF loader;
	tinygltf::Model    gltf;
	std::string        err, warn;

	// We load textures ourselves via AssetManager — disable tinygltf's own
	// image loading so it doesn't complain about a missing stb_image callback.
	loader.SetImageLoader(
	    [](tinygltf::Image*, const int, std::string*, std::string*,
	       int, int, const unsigned char*, int, void*) { return true; },
	    nullptr);

	const bool isBinary = path.size() >= 4 &&
	    path.substr(path.size() - 4) == ".glb";

	bool ok = isBinary
	    ? loader.LoadBinaryFromFile(&gltf, &err, &warn, path)
	    : loader.LoadASCIIFromFile(&gltf, &err, &warn, path);

	if (!warn.empty()) WARN_CORE("GltfLoader warning: {0}", warn);
	if (!ok) {
		ERROR_CORE("GltfLoader error: {0}", err.empty() ? path : err);
		return nullptr;
	}

	// Base directory of the .gltf file — used to resolve relative texture URIs
	const std::string baseDir = [&]() -> std::string {
		const auto sep = path.find_last_of("/\\");
		return sep != std::string::npos ? path.substr(0, sep + 1) : "";
	}();

	// Pre-build all materials so mesh primitives can reference them by index
	std::vector<SPtr<Scn::Material>> materials;
	materials.reserve(gltf.materials.empty() ? 1 : gltf.materials.size());
	if (gltf.materials.empty()) {
		materials.push_back(MakeShared<Scn::Material>("default", "default"));
	} else {
		for (int i = 0; i < static_cast<int>(gltf.materials.size()); ++i)
			materials.push_back(BuildMaterial(gltf, i, baseDir));
	}

	auto model = MakeShared<Scn::Model>();
	for (auto& mat : materials)
		model->AddMaterial(mat);

	// Walk the default scene (or scene 0)
	const int sceneIdx = gltf.defaultScene >= 0 ? gltf.defaultScene : 0;
	if (sceneIdx < static_cast<int>(gltf.scenes.size())) {
		for (int rootNode : gltf.scenes[sceneIdx].nodes)
			ProcessNode(gltf, rootNode, glm::mat4(1.f), *model, materials);
	}

	return model;
}

} // namespace Engine

#endif // __EMSCRIPTEN__
