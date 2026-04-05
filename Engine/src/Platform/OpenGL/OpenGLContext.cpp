#include "pch.h"
#include "OpenGLContext.h"

#include <GLFW/glfw3.h>
#ifndef __EMSCRIPTEN__
#include <glad/glad.h>
#else
#include <GLES3/gl3.h>
#endif

namespace Engine {

	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		CORE_ASSERT(windowHandle, "Window handle is null!")
	}

	void OpenGLContext::Init()
	{
		glfwMakeContextCurrent(m_WindowHandle);
#ifndef __EMSCRIPTEN__
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		CORE_ASSERT(status, "Failed to initialize Glad!");
#endif

		INFO_CORE("OpenGL Info:");
		INFO_CORE("  Vendor: {0}", glGetString(GL_VENDOR));
		INFO_CORE("  Renderer: {0}", glGetString(GL_RENDERER));
		INFO_CORE("  Version: {0}", glGetString(GL_VERSION));

	}

	void OpenGLContext::SwapBuffers()
	{
		glfwSwapBuffers(m_WindowHandle);
	}

}
