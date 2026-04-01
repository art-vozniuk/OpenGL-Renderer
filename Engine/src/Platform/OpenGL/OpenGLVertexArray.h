#pragma once

#include "Engine/Renderer/VertexArray.h"

namespace Engine {

	class OpenGLVertexArray : public VertexArray
	{
	public:
		OpenGLVertexArray();
		virtual ~OpenGLVertexArray();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void AddVertexBuffer(const SPtr<VertexBuffer>& vertexBuffer) override;
		virtual void SetIndexBuffer(const SPtr<IndexBuffer>& indexBuffer) override;

		const std::vector<SPtr<VertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
		const SPtr<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }
	private:
		uint m_RendererID;
		uint m_VertexBufferIndex = 0;
		std::vector<SPtr<VertexBuffer>> m_VertexBuffers;
		SPtr<IndexBuffer> m_IndexBuffer;
	};

}
