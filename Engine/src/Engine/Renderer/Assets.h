#pragma once

#include "pch.h"
#include "Shader.h"
#include "Texture.h"
#include "../Scene.h"

namespace Engine {


	template<class T>
	class AssetCreator
	{
	public:
		virtual ~AssetCreator() = default;
		
		virtual SPtr<T> Get(const std::string& name) = 0;

	protected:
		std::unordered_map<std::string, SPtr<T>> m_Data;
	};


	class ShaderCreator final : public AssetCreator<Shader>
	{
	public:
		virtual SPtr<Shader> Get(const std::string& name) override;
	};


	class TextureCreator final : public AssetCreator<Texture2D>
	{
	public:
		virtual SPtr<Texture2D> Get(const std::string& name) override;
	};


	class CubemapCreator final : public AssetCreator<CubeMap>
	{
	public:
		virtual SPtr<CubeMap> Get(const std::string& name) override;
	};


	class ModelCreator final : public AssetCreator<Scn::Model>
	{
	public:
		virtual SPtr<Scn::Model> Get(const std::string& name) override;
	};


	class AssetManager
	{
	private:
		AssetManager() = default;

	public:
		static SPtr<Shader>      GetShader(const std::string& name) { return Instance().m_ShaderCreator.Get(name); }
		static SPtr<Texture2D>   GetTexture2D(const std::string& name) { return Instance().m_TextureCreator.Get(name); }
		static SPtr<CubeMap>     GetCubemap(const std::string& name) { return Instance().m_CubemapCreator.Get(name); }
		static SPtr<Scn::Model>  GetModel(const std::string& name) { return Instance().m_ModelCreator.Get(name); }

	private:
		static AssetManager& Instance();

		ShaderCreator  m_ShaderCreator;
		TextureCreator m_TextureCreator;
		CubemapCreator m_CubemapCreator;
		ModelCreator   m_ModelCreator;
	};

}
