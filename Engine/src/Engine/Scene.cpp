#include "pch.h"
#include "Scene.h"
#include "Renderer/Renderer.h"
#include "Renderer/Assets.h"

namespace Engine {
	namespace Scn {


#ifndef __EMSCRIPTEN__
		static void VecAssimpToGlm(const aiVector3D& aVec, glm::vec3& gVec)
		{
			for (uint i = 0; i < 3; ++i) {
				gVec[i] = (float)aVec[i];
			}
		}


		static void MatAssimpToGlm(const aiMatrix4x4& aMat, glm::mat4& gMat)
		{
			for (uint i = 0; i < 4; ++i) {
				for (uint j = 0; j < 4; ++j)
					gMat[i][j] = (float)aMat[i][j];
			}
		}


		Model::Model(const aiScene* scene)
		{
			m_Textures.reserve(scene->mNumTextures);
			for (uint i = 0; i < scene->mNumTextures; ++i) {
				aiTexture* aiTex = scene->mTextures[i];
				auto& t = m_Textures.emplace_back(MakeShared<Texture>(aiTex->mName));
				t->Load();
			}

			m_Materials.reserve(scene->mNumMaterials);
			for (uint i = 0; i < scene->mNumMaterials; ++i) {
				m_Materials.emplace_back(MakeShared<Material>(scene->mMaterials[i], *this));
			}

			m_Meshes.reserve(scene->mNumMeshes);
			ProcessNode(scene->mRootNode, scene);
		}
#endif // !__EMSCRIPTEN__


		SPtr<Material> Model::GetMaterial(const std::string& name)
		{
			for (auto& m : m_Materials) {
				if (m->GetName() == name) {
					return m;
				}
			}
			ASSERT_FAIL("No material {0}", name);
			return nullptr;
		}


		SPtr<Texture> Model::AddTexture(const std::string& matName, const SPtr<Engine::Texture>& texture, Texture::Type type)
		{
			auto tex = MakeShared<Engine::Scn::Texture>(texture);
			tex->Load();
			return AddTexture(matName, tex, type);
		}


		SPtr<Texture> Model::AddTexture(const std::string& matName, const SPtr<Texture>& tex, Texture::Type type)
		{
			tex->SetType(type);
			GetMaterial(matName)->AddTexture(tex);
			return tex;
		}

		void Model::SetTransform(const glm::mat4& transform)
		{
			m_Transform = transform;
			for (auto& m : m_Meshes)
				m->SetParentTransform(transform);
		}


#ifndef __EMSCRIPTEN__
		void Model::ProcessNode(const aiNode* node, const aiScene* scene)
		{
			glm::mat4 transform;
			MatAssimpToGlm(node->mTransformation, transform);
			for (uint i = 0; i < node->mNumMeshes; ++i) {
				aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
				m_Meshes.emplace_back(MakeShared<Mesh>(mesh, transform, m_Transform, m_Materials[mesh->mMaterialIndex]));
			}

			for (uint i = 0; i < node->mNumChildren; ++i) {
				ProcessNode(node->mChildren[i], scene);
			}
		}


		Mesh::Mesh(const aiMesh* mesh, const glm::mat4& transform, const glm::mat4& parentTransform, const SPtr<Material>& material)
			: m_LocalTransform(transform)
			, m_Name(mesh->mName.C_Str())
			, m_Material(material)
		{
			SetParentTransform(parentTransform);

			m_Verts.reserve(mesh->mNumVertices);
			m_Faces.reserve(mesh->mNumFaces);
			for (uint i = 0; i < mesh->mNumVertices; ++i) {
				glm::vec3 position, normal, tangent, bitangent;
				glm::vec2 uv;
				VecAssimpToGlm(mesh->mVertices[i], position);
				VecAssimpToGlm(mesh->mNormals[i], normal);
				VecAssimpToGlm(mesh->mTangents[i], tangent);
				VecAssimpToGlm(mesh->mBitangents[i], bitangent);
				if (mesh->mTextureCoords[0]) {
					uv.x = (float)mesh->mTextureCoords[0][i].x;
					uv.y = (float)mesh->mTextureCoords[0][i].y;
				}
				m_Verts.emplace_back(position, normal, tangent, bitangent, uv);
			}
			for (uint i = 0; i < mesh->mNumFaces; ++i) {
				Face& f = m_Faces.emplace_back();
				aiFace& aiF = mesh->mFaces[i];
				f.indecies.reserve(aiF.mNumIndices);
				for (uint j = 0; j < aiF.mNumIndices; ++j) {
					f.indecies.emplace_back(aiF.mIndices[j]);
				}
			}

			SetupRenderable();
		}
#endif // !__EMSCRIPTEN__


		// Programmatic constructor: used by GltfLoader (web) or any future procedural loader
		Mesh::Mesh(std::vector<Vertex>&& verts, std::vector<Face>&& faces,
		           const glm::mat4& transform, const SPtr<Material>& material)
			: m_LocalTransform(transform)
			, m_Material(material)
		{
			m_WorldTransform = transform;
			m_Verts = std::move(verts);
			m_Faces = std::move(faces);
			SetupRenderable();
		}


		void Mesh::SetupRenderable(void)
		{
			m_VAO.reset(VertexArray::Create());

			SPtr<VertexBuffer> vbo;
			float* rawVerts = reinterpret_cast<float*>(m_Verts.data());
			vbo.reset(VertexBuffer::Create(rawVerts, (uint)m_Verts.size() * sizeof(Vertex)));
			vbo->SetLayout(GetVboLayout());
			m_VAO->AddVertexBuffer(vbo);

			std::vector<uint> inds;
			GetIndecies(inds);
			SPtr<IndexBuffer> ebo;
			ebo.reset(Engine::IndexBuffer::Create(inds.data(), (uint)inds.size()));
			m_VAO->SetIndexBuffer(ebo);
		}


		Engine::BufferLayout Mesh::GetVboLayout(void) const
		{
			return {
			   { Engine::ShaderDataType::Float3, "a_Position" },
			   { Engine::ShaderDataType::Float3, "a_Normal" },
			   { Engine::ShaderDataType::Float3, "a_Tangent" },
			   { Engine::ShaderDataType::Float3, "a_Bitangent" },
			   { Engine::ShaderDataType::Float2, "a_UV" }
			};
		}


		void Mesh::Render(void) const
		{

		}


		void Mesh::Render(const SPtr<Shader>& shader) const
		{
			shader->Bind();
			PrepareSubmit(shader);
			Renderer::Submit(shader, m_VAO, m_WorldTransform);
		}


		void Mesh::PrepareSubmit(const SPtr<Shader>& shader) const
		{
			int normalMapping = 0;
			int specularMapping = 0;
			for (const auto& t : m_Material->GetTextures()) {
				switch (t->GetType())
				{
				case Texture::Type::None:
				case Texture::Type::Ambient:
					break;
				case Texture::Type::Diffuse:
					shader->UploadUniformInt("u_material.diffuse", 0);
					t->GetRenderTex()->Bind(0);
					break;
				case Texture::Type::Specular:
					shader->UploadUniformInt("u_material.specular", 1);
					t->GetRenderTex()->Bind(1);
					specularMapping = 1;
					break;
				case Texture::Type::Normal:
					shader->UploadUniformInt("u_material.normal", 2);
					t->GetRenderTex()->Bind(2);
					normalMapping = 1;
					break;
				case Texture::Type::Bump:
					shader->UploadUniformInt("u_material.normal", 2);
					t->GetRenderTex()->Bind(2);
					normalMapping = 1;
					break;
				case Texture::Type::Reflection:
					shader->UploadUniformInt("u_material.reflection", 3);
					t->GetRenderTex()->Bind(3);
					break;
				case Texture::Type::Cubemap:
					m_VAO->Bind();
					t->GetRenderTex()->Bind();
					return;
					break;
				}
			}

			shader->UploadUniformFloat("u_material.shininess", 32.f);
			shader->UploadUniformInt("u_normalMapping", normalMapping);
			shader->UploadUniformInt("u_specularMapping", specularMapping);
		}


		void Mesh::GetIndecies(std::vector<uint>& indicies) const
		{
			if (m_Faces.empty())
				return;

			indicies.reserve(m_Faces.size() * m_Faces[0].indecies.size());
			for (const Face& f : m_Faces) {
				indicies.insert(indicies.end(), f.indecies.begin(), f.indecies.end());
			}
		}


		Engine::SPtr<Texture> Mesh::AddTexture(const SPtr<Engine::Texture>& tex, Texture::Type type)
		{
			auto texture = MakeShared<Texture>(tex);
			texture->SetType(type);
			m_Material->AddTexture(texture);
			return texture;
		}

		void Mesh::BindCubemap(const SPtr<CubeMap>& cubemap)
		{
			m_VAO->Bind();
			cubemap->Bind();
		}

#ifndef __EMSCRIPTEN__
		Material::Material(const aiMaterial* material, Model& model, const std::string& shader)
			: m_Shader(shader)
			, m_Name(material->mName)
		{
			LoadTextures(material, model, Texture::Type::Ambient);
			LoadTextures(material, model, Texture::Type::Diffuse);
			LoadTextures(material, model, Texture::Type::Specular);
			LoadTextures(material, model, Texture::Type::Normal);
			LoadTextures(material, model, Texture::Type::Bump);
		}


		void Material::LoadTextures(const aiMaterial* material, Model& model, Texture::Type type)
		{
			aiTextureType aiType = Texture::ConvertType(type);
			const uint count = material->GetTextureCount(aiType);
			for (uint i = 0; i < count; ++i) {
				aiString path;
				material->GetTexture(aiType, i, &path);
				std::string spath{ path.C_Str() };
				const uint64 pos = spath.find('*');
				SPtr<Texture> texture;
				if (pos != std::string::npos) {
					const uint id = std::stoi(spath.substr(pos + 1, spath.length() - pos - 1));
					texture = model.GetTexture(id);
				}
				else {
					auto tex = std::dynamic_pointer_cast<Engine::Texture, Texture2D>(AssetManager::GetTexture2D(spath));
					texture = MakeShared<Texture>(tex);
				}
				texture->SetType(type);
				m_Textures.emplace_back(texture);
			}
		}


		aiTextureType Texture::ConvertType(Type type)
		{
			switch (type)
			{
			case Texture::Type::Ambient:  return aiTextureType_AMBIENT;  break;
			case Texture::Type::Diffuse:  return aiTextureType_DIFFUSE;  break;
			case Texture::Type::Specular: return aiTextureType_SPECULAR; break;
			case Texture::Type::Normal:   return aiTextureType_NORMALS;  break;
			case Texture::Type::Bump:     return aiTextureType_HEIGHT;	 break;

			default: ASSERT_FAIL("Unsupported type"); break;
			}
			return aiTextureType_UNKNOWN;
		}
#endif // !__EMSCRIPTEN__


		void Texture::Load(void)
		{
			if (!IsLoaded()) {
				m_RenderTex = AssetManager::GetTexture2D(m_Name); //Texture2D::Create(path);
			}
		}


		void PrimitiveMesh::Init(const glm::mat4& transform, float scale /*= 1.f*/)
		{
			SetTransform(transform);

			FillVerticies();
			for (auto& v : m_Verts)
				v.position *= scale;

			FillIndicies();

			SetupRenderable();
		}


		void Cube::FillVerticies(void)
		{
			glm::vec3 v3(0.f);
			glm::vec2 v2(0.f);
			m_Verts = {
			   { { -1.f, -1.f,  1.f }, v3, v3, v3, v2 },
			   { {  1.f, -1.f,  1.f }, v3, v3, v3, v2 },
			   { {  1.f,  1.f,  1.f }, v3, v3, v3, v2 },
			   { { -1.f,  1.f,  1.f }, v3, v3, v3, v2 },
			   { {  1.f, -1.f, -1.f }, v3, v3, v3, v2 },
			   { {  1.f,  1.f, -1.f }, v3, v3, v3, v2 },
			   { { -1.f,  1.f, -1.f }, v3, v3, v3, v2 },
			   { { -1.f, -1.f, -1.f }, v3, v3, v3, v2 },
			};
		}


		void Cube::FillIndicies(void)
		{
			m_Faces = {
			   { 0, 1, 3 },
			   { 1, 2, 3 },
			   { 1, 5, 2 },
			   { 1, 4, 5 },
			   { 6, 5, 4 },
			   { 6, 4, 7 },
			   { 0, 3, 6 },
			   { 0, 6, 7 },
			   { 3, 2, 5 },
			   { 3, 5, 6 },
			   { 0, 4, 1 },
			   { 0, 7, 4 }
			};
		}


		void Quad::FillVerticies(void)
		{
			glm::vec3 v3(0.f);
			m_Verts = {
			   { { -1.f, -1.f,  0.f }, v3, v3, v3, { 0, 0 } },
			   { {  1.f, -1.f,  0.f }, v3, v3, v3, { 1, 0 } },
			   { {  1.f,  1.f,  0.f }, v3, v3, v3, { 1, 1 } },
			   { { -1.f,  1.f,  0.f }, v3, v3, v3, { 0, 1 } },
			};
		}

		void Quad::FillIndicies(void)
		{
			m_Faces = {
			   { 0, 1, 2 },
			   { 2, 3, 0 }
			};
		}


		// Engine::BufferLayout SkyBox::GetVboLayout(void) const
		// {
		//    return {
		//       { Engine::ShaderDataType::Float3, "a_Position" }
		//    };
		// }


		// void SkyBox::FillVerticies(void)
		// {
		//    glm::vec3 v3(0.f);
		//    glm::vec2 v2(0.f);
		//    m_Verts = {
		//       { { -1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f,  1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f, -1.0f }, v3, v3, v3, v2 },
		//       { { -1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//       { {  1.0f, -1.0f,  1.0f }, v3, v3, v3, v2 },
		//    };
		// }



		// void SkyBox::FillIndicies(void)
		// {
		//    m_Faces = {
		//       { 0, 3, 1 },
		//       { 1, 3, 2 },
		//       { 1, 2, 5 },
		//       { 1, 5, 4 },
		//       { 6, 4, 5 },
		//       { 6, 7, 4 },
		//       { 0, 6, 3 },
		//       { 0, 7, 6 },
		//       { 3, 5, 2 },
		//       { 3, 6, 5 },
		//       { 0, 1, 4 },
		//       { 0, 4, 7 }
		//    };
		// }
	}
}
