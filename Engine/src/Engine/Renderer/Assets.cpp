#include "pch.h"
#include "Assets.h"
#include "Renderer.h"
#include "GltfLoader.h"

#include <filesystem>


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

		// Shader sources are platform-agnostic .glsl files. The
		// ShaderPreprocessor injects the correct #version / precision
		// header per platform and resolves #include "..." directives
		// relative to the shaders/ folder.
		static const fs::path shadersPath = GetAssetsDir() / "shaders";
		SPtr<Shader> shader;
		shader.reset(Shader::Create(
			(shadersPath / (name + "_v.glsl")).string(),
			(shadersPath / (name + "_f.glsl")).string(),
			shadersPath.string()));
		m_Data.emplace(name, shader);
		return shader;
	}


	SPtr<Texture2D> TextureCreator::Get(const std::string& name)
	{
		const std::string normalizedPath = NormalizeAssetPath(name);
		const std::string filename = fs::path(normalizedPath).filename().string();

		const auto it = m_Data.find(filename);
		if (it != m_Data.end()) {
			return it->second;
		}

		// GltfLoader resolves each texture URI against the glTF's base
		// directory and passes the full path in, so we always receive an
		// absolute path here.
		auto tex = Texture2D::Create(normalizedPath);
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
		auto model = GltfLoader::Load(path.string());
		ASSERT(model, "glTF model loading failed: {0}", path.string());

		m_Data.emplace(name, model);
		return model;
	}


	Engine::AssetManager& AssetManager::Instance(void)
	{
		static AssetManager instance;
		return instance;
	}

}
