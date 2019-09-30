#pragma once

#include <string>

#include "Engine/Core.h"

namespace Engine {

	class Texture
	{
	public:
		virtual ~Texture(void) = default;

		virtual uint32_t		GetWidth	(void) const = 0;
		virtual uint32_t		GetHeight	(void) const = 0;

		virtual void			Bind		(uint32_t slot = 0) const = 0;

		virtual unsigned int	GetRenderId	(void) const = 0;
	};

	class Texture2D : public Texture
	{
	public:
		static SPtr<Texture2D> Create(const std::string& path);
		static SPtr<Texture2D> Create(unsigned char* data, int width, int height, int channels);
	};

}