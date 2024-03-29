#pragma once

#include <glm/glm.hpp>

#include "VertexArray.h"

namespace Engine {

	class RendererAPI
	{
	public:
		enum class API
		{
			None = 0,
			OpenGL = 1,
			Vulkan = 2,
		};
	public:
		virtual void Init(void) = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void Clear(void) = 0;
		virtual void DepthMask(bool enable) = 0;
		virtual void CullFaces(bool enable) = 0;

		virtual void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray) = 0;
		virtual void Draw(const std::shared_ptr<VertexArray>& vertexArray) = 0;

		inline static API GetAPI() { return s_API; }
	private:
		static API s_API;
	};

}