#pragma once
#include "Texture.h"

namespace Engine {

	enum class ShaderDataType
	{
		None = 0,
		Float,
		Float2,
		Float3,
		Float4,
		Mat3,
		Mat4,
		Int,
		Int2,
		Int3,
		Int4,
		Bool
	};

	static constexpr uint ShaderDataTypeSize(ShaderDataType type)
	{
		uint size = 0;
		switch (type) {
		case ShaderDataType::None:     break;
		case ShaderDataType::Float:    size = 4;		 break;
		case ShaderDataType::Float2:   size = 4 * 2;	 break;
		case ShaderDataType::Float3:   size = 4 * 3;	 break;
		case ShaderDataType::Float4:   size = 4 * 4;	 break;
		case ShaderDataType::Mat3:     size = 4 * 3 * 3; break;
		case ShaderDataType::Mat4:     size = 4 * 4 * 4; break;
		case ShaderDataType::Int:      size = 4;		 break;
		case ShaderDataType::Int2:     size = 4 * 2;	 break;
		case ShaderDataType::Int3:     size = 4 * 3;	 break;
		case ShaderDataType::Int4:     size = 4 * 4;	 break;
		case ShaderDataType::Bool:     size = 1;		 break;
		}

		//static_assert(size);
		return size;
	}

	struct BufferElement
	{
		std::string Name;
		ShaderDataType Type;
		uint Size;
		uint Offset;
		bool Normalized;

		BufferElement() {}

		BufferElement(ShaderDataType type, const std::string& name, bool normalized = false)
			: Name(name), Type(type), Size(ShaderDataTypeSize(type)), Offset(0), Normalized(normalized)
		{
		}

		uint GetComponentCount() const
		{
			switch (Type)
			{
			case ShaderDataType::None:    break;
			case ShaderDataType::Float:   return 1;
			case ShaderDataType::Float2:  return 2;
			case ShaderDataType::Float3:  return 3;
			case ShaderDataType::Float4:  return 4;
			case ShaderDataType::Mat3:    return 3 * 3;
			case ShaderDataType::Mat4:    return 4 * 4;
			case ShaderDataType::Int:     return 1;
			case ShaderDataType::Int2:    return 2;
			case ShaderDataType::Int3:    return 3;
			case ShaderDataType::Int4:    return 4;
			case ShaderDataType::Bool:    return 1;
			}

			CORE_ASSERT(false, "Unknown ShaderDataType!");
			return 0;
		}
	};

	class BufferLayout
	{
	public:
		BufferLayout() {}

		BufferLayout(const std::initializer_list<BufferElement>& elements)
			: m_Elements(elements)
		{
			CalculateOffsetsAndStride();
		}

		inline uint GetStride() const { return m_Stride; }
		inline const std::vector<BufferElement>& GetElements() const { return m_Elements; }

		std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
		std::vector<BufferElement>::iterator end() { return m_Elements.end(); }
		std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
		std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }
	private:
		void CalculateOffsetsAndStride()
		{
			uint offset = 0;
			m_Stride = 0;
			for (auto& element : m_Elements)
			{
				element.Offset = offset;
				offset += element.Size;
				m_Stride += element.Size;
			}
		}
	private:
		std::vector<BufferElement> m_Elements;
		uint m_Stride = 0;
	};

	class VertexBuffer
	{
	public:
		virtual ~VertexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual const BufferLayout& GetLayout() const = 0;
		virtual void SetLayout(const BufferLayout& layout) = 0;

		virtual uint GetSize() const = 0;

		static VertexBuffer* Create(float* vertices, uint size);
	};

	class IndexBuffer
	{
	public:
		virtual ~IndexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual uint GetCount() const = 0;

		static IndexBuffer* Create(uint* indices, uint size);
	};

	class RenderBuffer;

	class FrameBuffer
	{
	public:
		virtual ~FrameBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual void AddTexture(const SPtr<Texture>& texture) = 0;
		virtual void AddRenderBuffer(const SPtr<RenderBuffer>& rb) = 0;

		virtual bool Check() const = 0;

		static FrameBuffer* Create();
	};


	class RenderBuffer
	{
	public:
		virtual ~RenderBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		static RenderBuffer* Create(uint windth, uint heigth);
	};


}
