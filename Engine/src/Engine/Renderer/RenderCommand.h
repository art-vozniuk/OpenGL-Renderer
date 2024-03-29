#pragma once

#include "RendererAPI.h"

namespace Engine {

	class RenderCommand
	{
	public:
		inline static void Init()
		{
			s_RendererAPI->Init();
		}

		inline static void SetClearColor(const glm::vec4& color)
		{
			s_RendererAPI->SetClearColor(color);
		}

		inline static void Clear()
		{
			s_RendererAPI->Clear();
		}

		inline static void DepthMask(bool enable)
		{
			s_RendererAPI->DepthMask(enable);
		}

		inline static void CullFaces(bool enable)
		{
			s_RendererAPI->CullFaces(enable);
		}

		inline static void DrawIndexed(const std::shared_ptr<VertexArray>& vertexArray)
		{
			s_RendererAPI->DrawIndexed(vertexArray);
		}

		inline static void Draw(const std::shared_ptr<VertexArray>& vertexArray)
		{
			s_RendererAPI->Draw(vertexArray);
		}
	private:
		static RendererAPI* s_RendererAPI;
	};

}
