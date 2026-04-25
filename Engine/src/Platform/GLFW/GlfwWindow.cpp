#include "pch.h"
#include "GlfwWindow.h"

#include "Engine/Events/ApplicationEvent.h"
#include "Engine/Events/MouseEvent.h"
#include "Engine/Events/KeyEvent.h"

namespace Engine {

	static bool s_GLFWInitialized = false;

	static void GLFWErrorCallback(int error, const char* description)
	{
		ERROR_CORE("GLFW Error ({0}): {1}", error, description);
	}

	Window* Window::Create(const WindowProps& props)
	{
		return new GlfwWindow(props);
	}

	GlfwWindow::GlfwWindow(const WindowProps& props)  { Init(props); }
	GlfwWindow::~GlfwWindow()                         { Shutdown(); }

	void GlfwWindow::Init(const WindowProps& props)
	{
		m_Data.Title  = props.Title;
		m_Data.Width  = props.Width;
		m_Data.Height = props.Height;

		INFO_CORE("Creating window {0} ({1}, {2})", props.Title, props.Width, props.Height);

		if (!s_GLFWInitialized)
		{
			int success = glfwInit();
			CORE_ASSERT(success, "Could not initialise GLFW!");
			glfwSetErrorCallback(GLFWErrorCallback);
			s_GLFWInitialized = true;
		}

		// WebGPU port: window must NOT carry an OpenGL context. GLFW_NO_API
		// is required so glfwCreateWindow doesn't bind a GL state, and so
		// the surface picker (glfw3webgpu) can attach a Metal / Vulkan /
		// canvas surface instead.
		glfwDefaultWindowHints();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);

		m_Window = glfwCreateWindow((int)props.Width, (int)props.Height,
		                             m_Data.Title.c_str(), nullptr, nullptr);

		glfwSetWindowUserPointer(m_Window, &m_Data);

		// Callbacks. Identical to the GL version — pure event plumbing,
		// no GL state involved.
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			data.Width = width;
			data.Height = height;
			WindowResizeEvent event(width, height);
			data.EventCallback(event);
		});

		glfwSetFramebufferSizeCallback(m_Window, [](GLFWwindow* window, int width, int height) {
			// Forward framebuffer size as a resize event too — on hi-DPI
			// (Retina) Mac it differs from the window's logical size and
			// is what WebGPU surface configuration actually needs.
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			data.Width = width;
			data.Height = height;
			WindowResizeEvent event(width, height);
			data.EventCallback(event);
		});

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			WindowCloseEvent event;
			data.EventCallback(event);
		});

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			switch (action) {
				case GLFW_PRESS:   { KeyPressedEvent  e(key, 0); data.EventCallback(e); break; }
				case GLFW_RELEASE: { KeyReleasedEvent e(key);    data.EventCallback(e); break; }
				case GLFW_REPEAT:  { KeyPressedEvent  e(key, 1); data.EventCallback(e); break; }
			}
		});

		glfwSetCharCallback(m_Window, [](GLFWwindow* window, uint keycode) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			KeyTypedEvent event(keycode);
			data.EventCallback(event);
		});

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int /*mods*/) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			switch (action) {
				case GLFW_PRESS:   { MouseButtonPressedEvent  e(button); data.EventCallback(e); break; }
				case GLFW_RELEASE: { MouseButtonReleasedEvent e(button); data.EventCallback(e); break; }
			}
		});

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			MouseScrolledEvent event((float)xOffset, (float)yOffset);
			data.EventCallback(event);
		});

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double xPos, double yPos) {
			WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
			MouseMovedEvent event((float)xPos, (float)yPos);
			data.EventCallback(event);
		});
	}

	void GlfwWindow::Shutdown()
	{
		if (m_Window) glfwDestroyWindow(m_Window);
	}

	void GlfwWindow::OnUpdate()
	{
		// Surface present is owned by the WebGPU renderer, not the window.
		// We just pump GLFW events here.
		glfwPollEvents();
	}

	void GlfwWindow::SetVSync(bool enabled)
	{
		// VSync is controlled by the swap-chain present mode (Fifo) in
		// WGPUContext now. Kept as a no-op for ABI compat with old callers.
		m_Data.VSync = enabled;
	}

	bool GlfwWindow::IsVSync() const { return m_Data.VSync; }

}
