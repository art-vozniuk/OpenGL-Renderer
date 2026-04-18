#include "pch.h"
#include "Scene.h"
#include "Renderer/Renderer.h"
#include "Renderer/Assets.h"

namespace Engine {
	namespace Scn {


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
