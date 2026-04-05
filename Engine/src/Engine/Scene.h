#pragma once

#include "pch.h"
#ifndef __EMSCRIPTEN__
#include "assimp/scene.h"
#endif
#include "glm/gtx/mixed_product.hpp"
#include "Renderer/VertexArray.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"

namespace Engine {
	namespace Scn {

		class ModelPart;
		class Mesh;
		class Material;
		class Texture;
		struct Vertex;
		struct Face;
		class Light;
		//class Animation;
		//class CameraSpot;


		class Texture
		{
		public:
			enum class Type {
				None,
				Ambient,
				Diffuse,
				Specular,
				Normal,
				Bump,
				Reflection,
				Cubemap,
			};

			Texture(const std::string& name)
				: m_Name(name) { }

			Texture(const SPtr<Engine::Texture>& tex)
				: m_RenderTex(tex) { }

#ifndef __EMSCRIPTEN__
			static aiTextureType ConvertType(Type type);
#endif
			bool IsLoaded(void) const { return m_RenderTex.get(); }
			void Load(void);
			const std::string& GetName(void) const { return m_Name; }
			void SetType(Type type) { m_Type = type; }
			Type GetType(void) const { return m_Type; }
			SPtr<Engine::Texture>	GetRenderTex(void) { return m_RenderTex; }

		private:
			Type m_Type = Type::None;
			std::string m_Name;

			SPtr<Engine::Texture> m_RenderTex;
		};


		class Mesh
		{
		public:
			Mesh(void) {};
			// Programmatic constructor (used by tinygltf loader on web)
			Mesh(std::vector<Vertex>&& verts, std::vector<Face>&& faces,
			     const glm::mat4& transform, const SPtr<Material>& material);
#ifndef __EMSCRIPTEN__
			Mesh(const aiMesh* mesh, const glm::mat4& transform, const glm::mat4& parentTransform, const SPtr<Material>& material);
#endif

			virtual ~Mesh() = default;

			void SetParentTransform(const glm::mat4& transform) { m_WorldTransform = transform * m_LocalTransform; }
			void Render(void) const;
			void Render(const SPtr<Engine::Shader>& shader) const;
			void GetIndecies(std::vector<uint>& indicies) const;
			SPtr<Texture> AddTexture(const SPtr<Engine::Texture>& tex, Texture::Type type);
			void BindCubemap(const SPtr<CubeMap>& cubemap);

		protected:
			virtual BufferLayout GetVboLayout(void) const;
			virtual void PrepareSubmit(const SPtr<Shader>& shader) const;
			void SetupRenderable(void);

			glm::mat4 m_WorldTransform = glm::mat4(1.f);
			std::vector<Vertex>	m_Verts;
			std::vector<Face> m_Faces;

		private:
			std::string m_Name;
			glm::mat4 m_LocalTransform = glm::mat4(1.f);
			SPtr<Material> m_Material = MakeShared<Material>();
			SPtr<VertexArray> m_VAO;
		};


		class Model
		{
		public:
			// Programmatic constructor (used by tinygltf loader on web)
			Model() = default;
#ifndef __EMSCRIPTEN__
			Model(const aiScene* scene);
#endif

			void AddMesh(const SPtr<Mesh>& mesh) { m_Meshes.push_back(mesh); }
			void AddMaterial(const SPtr<Material>& material) { m_Materials.push_back(material); }

			SPtr<Texture> GetTexture(uint idx) { return m_Textures[idx]; }
			void Render(void) { for (auto& m : m_Meshes) m->Render(); }
			void Render(const SPtr<Shader>& shader) { for (auto& m : m_Meshes) m->Render(shader); }

			SPtr<Material> GetMaterial(const std::string& name);
			SPtr<Texture> AddTexture(const std::string& matName, const SPtr<Engine::Texture>& texture, Texture::Type type);
			SPtr<Texture> AddTexture(const std::string& matName, const SPtr<Texture>& tex, Texture::Type type);
			void BindCubemap(const SPtr<CubeMap>& cubemap) { for (auto& m : m_Meshes) m->BindCubemap(cubemap); }

			void SetTransform(const glm::mat4& transform);

		private:
#ifndef __EMSCRIPTEN__
			void ProcessNode(const aiNode* node, const aiScene* scene);
#endif

			glm::mat4 m_Transform = glm::mat4(1.f);

			std::vector<SPtr<Mesh>>		   m_Meshes;
			std::vector<SPtr<Material>>   m_Materials;
			std::vector<SPtr<Texture>>    m_Textures;
		};


		class Material
		{
		public:
			using TextureList = std::vector<SPtr<Texture>>;

			Material(void) : m_Shader("default"), m_Name("Material") { }
			// Programmatic constructor (used by tinygltf loader on web)
			Material(const std::string& name, const std::string& shader = "default")
				: m_Shader(shader), m_Name(name) { }
#ifndef __EMSCRIPTEN__
			Material(const aiMaterial* material, Model& model, const std::string& shader = "default");
#endif

			const TextureList& GetTextures(void) const { return m_Textures; }
			const std::string& GetName(void) const { return m_Name; }

			void AddTexture(const SPtr<Texture>& tex) { m_Textures.emplace_back(tex); }

		private:
#ifndef __EMSCRIPTEN__
			void LoadTextures(const aiMaterial* material, Model& model, Texture::Type type);
#endif

			std::string m_Shader;
			std::string m_Name;
			TextureList m_Textures;
		};


		struct Vertex
		{
			Vertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec3& tangent, const glm::vec3& bitangent, const glm::vec2& uv)
				: position(position)
				, normal(normal)
				, tangent(tangent)
				, bitangent(bitangent)
				, uv(uv) { }

			glm::vec3 position;
			glm::vec3 normal;
			glm::vec3 tangent;
			glm::vec3 bitangent;
			glm::vec2 uv;
		};


		struct Face
		{
			Face(void) {};
			Face(uint i0, uint i1, uint i2) { indecies = { i0, i1, i2 }; };

			std::vector<uint> indecies;
		};


		class PrimitiveMesh : public Mesh
		{
		public:
			void Init(const glm::mat4& transform, float scale = 1.f);

			void SetTransform(const glm::mat4& transform) { m_WorldTransform = transform; }

		protected:
			virtual void FillVerticies(void) = 0;
			virtual void FillIndicies(void) = 0;
		};


		class Cube : public PrimitiveMesh
		{
		public:
			Cube(const glm::mat4& transform, float scale = 1.f) { Init(transform, scale); }

		protected:
			//virtual void PrepareSubmit (const SPtr<Shader>& shader) const override {}
			virtual void FillVerticies(void) override;
			virtual void FillIndicies(void) override;
		};


		class Quad : public PrimitiveMesh
		{
		public:
			Quad(const glm::mat4& transform, float scale = 1.f) { Init(transform, scale); }

		protected:
			virtual void FillVerticies(void) override;
			virtual void FillIndicies(void) override;
			virtual void PrepareSubmit(const SPtr<Shader>& shader) const override { Mesh::PrepareSubmit(shader); }
		};


		class SkyBox : public Cube
		{
		public:
			SkyBox() : Cube(glm::mat4(1.f)) {}

		protected:
			//virtual BufferLayout GetVboLayout(void) const override;
			//virtual void         FillVerticies(void) override;
			//virtual void         FillIndicies(void) override;
		};


	}
}
