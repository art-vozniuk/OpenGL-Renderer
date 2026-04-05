#include "pch.h"
#include "OpenGLTexture.h"

#include "stb_image.h"

#ifndef __EMSCRIPTEN__
#include <glad/glad.h>
#include "assimp/texture.h"
#else
#include <GLES3/gl3.h>
#endif

namespace Engine {

	OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
		: m_Path(path)
	{
		int width, height, channels;
		//stbi_set_flip_vertically_on_load(1);
		stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, 0);
		if (!data)
		{
			ERROR_CORE("Texture failed to load at path: {0}", path);
			static unsigned char fallback[] = { 0, 0, 0, 255 };
			Init(fallback, 1, 1, 4);
			return;
		}

		Init(data, width, height, channels);
		stbi_image_free(data);
	}

	OpenGLTexture2D::~OpenGLTexture2D()
	{
		glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLTexture2D::Init(unsigned char* data, int width, int height, int channels)
	{
		m_Width = width;
		m_Height = height;

		GLenum internalFormat = 0, dataFormat = 0;

		switch (channels) {
		case 4: {
			internalFormat = GL_RGBA8;
			dataFormat = GL_RGBA;
		} break;
		case 3: {
			internalFormat = GL_RGB8;
			dataFormat = GL_RGB;
		} break;
		case 1: {
			internalFormat = GL_R8;
			dataFormat = GL_RED;
		} break;
		default:
			CORE_ASSERT(false, "Format not supported!");
		}

		glGenTextures(1, &m_RendererID);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, dataFormat, GL_UNSIGNED_BYTE, data);
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	void OpenGLTexture2D::Bind(uint slot) const
	{
		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_2D, m_RendererID);
	}

	OpenGLCubeMap::OpenGLCubeMap(const std::vector<std::string>& paths)
	{
		ASSERT(paths.size() == 6, "Cubemap requires 6 faces");

		glGenTextures(1, &m_RendererID);
		glBindTexture(GL_TEXTURE_CUBE_MAP, m_RendererID);

		stbi_set_flip_vertically_on_load(0);
		int width, height, channels;
		for (uint i = 0; i < paths.size(); i++)
		{
			unsigned char* data = stbi_load(paths[i].c_str(), &width, &height, &channels, 0);
			if (data)
			{
				m_Width = width;
				m_Height = height;
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
					0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data
				);
			}
			else
			{
				ERROR_CORE("Cubemap texture failed to load at path: {0}", paths[i]);
			}
			stbi_image_free(data);
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	}

	OpenGLCubeMap::~OpenGLCubeMap()
	{
		glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLCubeMap::Bind(uint slot /*= 0*/) const
	{
		glActiveTexture(GL_TEXTURE0 + slot);
		glBindTexture(GL_TEXTURE_CUBE_MAP, m_RendererID);
	}

}
