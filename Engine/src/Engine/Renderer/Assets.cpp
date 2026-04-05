#include "pch.h"
#include "Assets.h"
#include "Renderer.h"

#include <filesystem>
#ifndef __EMSCRIPTEN__
#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"
#else
#include "GltfLoader.h"
#endif


namespace Engine {

	namespace fs = std::filesystem;

	namespace {

		fs::path GetAssetsDir()
		{
			return fs::path(ENGINE_ASSETS_DIR);
		}

		std::string NormalizeAssetPath(std::string path)
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			return path;
		}

	}

	SPtr<Shader> ShaderCreator::Get(const std::string& name)
	{
		auto it = m_Data.find(name);
		if (it != m_Data.end()) {
			return it->second;
		}

		static const fs::path shadersPath = GetAssetsDir() / "shaders";
		std::string ext = GetShaderExt();
		SPtr<Shader> shader;
		shader.reset(Shader::Create(
			(shadersPath / (name + "_v" + ext)).string(),
			(shadersPath / (name + "_f" + ext)).string()));
		m_Data.emplace(name, shader);
		return shader;
	}


	std::string ShaderCreator::GetShaderExt(void)
	{
		switch (Renderer::GetAPI())
		{
		case RendererAPI::API::None:    CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return "";
#ifndef __EMSCRIPTEN__
		case RendererAPI::API::OpenGL:  return ".glsl";
#else
		case RendererAPI::API::OpenGL:  return "_es.glsl";
#endif
		}
		CORE_ASSERT(false, "Unknown RendererAPI!");
		return "";
	}


	SPtr<Texture2D> TextureCreator::Get(const std::string& name)
	{
		const std::string normalizedPath = NormalizeAssetPath(name);
		const std::string filename = fs::path(normalizedPath).filename().string();

		const auto it = m_Data.find(filename);
		if (it != m_Data.end()) {
			return it->second;
		}

		// If the caller passed a full / rooted path (e.g. from GltfLoader), use
		// it directly; otherwise fall back to the legacy textures/sponza location.
		fs::path resolved;
		if (fs::path(normalizedPath).has_parent_path() &&
		    normalizedPath.find('/') != std::string::npos)
		{
			resolved = fs::path(normalizedPath);
		}
		else
		{
			static const fs::path texturesPath = GetAssetsDir() / "textures" / "sponza";
			resolved = texturesPath / filename;
		}
		auto tex = Texture2D::Create(resolved.string());
		m_Data.emplace(filename, tex);

		return tex;
	}


	SPtr<CubeMap> CubemapCreator::Get(const std::string& name)
	{
		const auto it = m_Data.find(name);
		if (it != m_Data.end()) {
			return it->second;
		}

		static const fs::path texturesPath = GetAssetsDir() / "textures" / "cubemaps";
		const fs::path path = texturesPath / name;
		std::vector<std::string> names = {
		   (path / "posx.jpg").string(),
		   (path / "negx.jpg").string(),
		   (path / "posy.jpg").string(),
		   (path / "negy.jpg").string(),
		   (path / "posz.jpg").string(),
		   (path / "negz.jpg").string(),
		};
		auto tex = CubeMap::Create(names);
		m_Data.emplace(name, tex);

		return tex;
	}


	Engine::SPtr<Engine::Scn::Model> ModelCreator::Get(const std::string& name)
	{
		const auto it = m_Data.find(name);
		if (it != m_Data.end()) {
			return it->second;
		}

		const fs::path path = GetAssetsDir() / "models" / name;

#ifndef __EMSCRIPTEN__
		Assimp::Importer importer;
		const aiScene* scene = importer.ReadFile(path.string().c_str(), aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);
		ASSERT(scene, "Model loading failed: {0}", path.string());

		auto model = MakeShared<Scn::Model>(scene);
#else
		auto model = GltfLoader::Load(path.string());
		ASSERT(model, "glTF model loading failed: {0}", path.string());
#endif

		m_Data.emplace(name, model);
		return model;
	}


	Engine::AssetManager& AssetManager::Instance(void)
	{
		static AssetManager instance;
		return instance;
	}

}
