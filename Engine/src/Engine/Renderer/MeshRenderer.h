#pragma once

#include "Camera.h"
#include "GltfLoader.h"
#include "WGPUContext.h"
#include "../Core.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace Engine {

	// Renders a parsed glTF MeshData with minimal PBR (one directional
	// light, constant ambient). Each primitive becomes one draw; each
	// material becomes one bind group (uniform + 3 textures + sampler).
	//
	// Depth: the mesh pipeline declares Depth32Float and the caller is
	// expected to open a render pass with a matching depth attachment.
	// EditorScene owns the depth texture and uses OpenMeshPass() below.
	class MeshRenderer
	{
	public:
		struct AABB { glm::vec3 min, max; bool valid = false; };

		explicit MeshRenderer(WGPUContext& ctx);
		~MeshRenderer();

		MeshRenderer(const MeshRenderer&) = delete;
		MeshRenderer& operator=(const MeshRenderer&) = delete;

		// Reload GPU resources from a parsed mesh dataset.
		void Upload(const MeshData& data);

		void SetModelMatrix(const glm::mat4& m) { m_ModelMatrix = m; }
		const glm::mat4& ModelMatrix() const { return m_ModelMatrix; }

		const AABB& BoundingBox() const { return m_AABB; }
		glm::vec3   Centroid()    const { return (m_AABB.min + m_AABB.max) * 0.5f; }

		// Encode the per-primitive draws into `pass`. `lightDirWorld` is the
		// world-space direction TO the light (i.e. -lightDirection).
		void EncodeRender(WGPURenderPassEncoder pass,
		                  const SPtr<Camera>& camera,
		                  const glm::vec2& viewportSize,
		                  const glm::vec3& lightDirWorld,
		                  const glm::vec3& lightColor,
		                  const glm::vec3& ambient);

		size_t PrimitiveCount() const { return m_Primitives.size(); }

		// Depth texture format declared by this renderer's pipeline.
		// EditorScene must open the render pass with a matching depth view.
		static WGPUTextureFormat DepthFormat() { return WGPUTextureFormat_Depth24Plus; }

	private:
		struct GpuPrimitive
		{
			WGPUBuffer vbo  = nullptr;
			WGPUBuffer ibo  = nullptr;
			uint32_t   indexCount  = 0;
			uint32_t   vertexCount = 0;
			int        materialIndex = -1;
		};

		struct GpuMaterial
		{
			WGPUBuffer    uniform = nullptr;
			WGPUTexture   baseColorTex = nullptr;
			WGPUTextureView baseColorView = nullptr;
			WGPUTexture   normalTex = nullptr;
			WGPUTextureView normalView = nullptr;
			WGPUTexture   mrTex = nullptr;
			WGPUTextureView mrView = nullptr;
			WGPUBindGroup bindGroup = nullptr;
			glm::vec4 baseColorFactor = glm::vec4(1.0f);
			float metallicFactor  = 1.0f;
			float roughnessFactor = 1.0f;
			bool hasBaseColorTex = false;
			bool hasNormalTex    = false;
			bool hasMRTex        = false;
		};

		void CreatePipeline();
		void DestroyMeshData();
		void DestroyAll();
		WGPUTexture CreateRgba8Texture(const uint8_t* pixels, int w, int h,
		                               const char* label);

		WGPUContext* m_Ctx = nullptr;
		WGPURenderPipeline   m_Pipeline = nullptr;
		WGPUBindGroupLayout  m_BGL      = nullptr;
		WGPUPipelineLayout   m_PL       = nullptr;
		WGPUSampler          m_Sampler  = nullptr;

		// Two small 1x1 placeholders so a material missing a normal or MR
		// texture still satisfies the bind-group layout.
		WGPUTexture     m_DummyWhiteTex   = nullptr;
		WGPUTextureView m_DummyWhiteView  = nullptr;
		WGPUTexture     m_DummyNormalTex  = nullptr;
		WGPUTextureView m_DummyNormalView = nullptr;
		WGPUTexture     m_DummyMRTex      = nullptr;
		WGPUTextureView m_DummyMRView     = nullptr;

		std::vector<GpuPrimitive> m_Primitives;
		std::vector<GpuMaterial>  m_Materials;
		AABB      m_AABB;
		glm::mat4 m_ModelMatrix = glm::mat4(1.0f);
	};

}
