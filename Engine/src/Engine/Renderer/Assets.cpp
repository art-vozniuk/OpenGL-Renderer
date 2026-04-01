#include "pch.h"
#include "Assets.h"
#include "Renderer.h"

#include <filesystem>
#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"


namespace Engine {

	namespace fs = std::filesystem;

	namespace {

		fs::path GetAssetsDir()
		{
			return fs::path(ENGINE_ASSETS_DIR);
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
		case RendererAPI::API::None:    CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
		case RendererAPI::API::OpenGL:  return ".glsl";
		}
		CORE_ASSERT(false, "Unknown RendererAPI!");
		return nullptr;
	}


	SPtr<Texture2D> TextureCreator::Get(const std::string& name)
	{
		const std::string filename = fs::path(name).filename().string();

		const auto it = m_Data.find(filename);
		if (it != m_Data.end()) {
			return it->second;
		}

		static const fs::path texturesPath = GetAssetsDir() / "textures" / "sponza";
		auto tex = Texture2D::Create((texturesPath / filename).string());
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

		Assimp::Importer importer;
		const aiScene* scene = importer.ReadFile(path.string().c_str(), aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);
		ASSERT(scene, "Model loading failed: {0}", path.string());

		auto model = MakeShared<Scn::Model>(scene);
		m_Data.emplace(name, model);

		return model;
	}


	Engine::AssetManager& AssetManager::Instance(void)
	{
		static AssetManager instance;
		return instance;
	}

}
