#include "pch.h"
#include "Application.h"

#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Input.h"
#include "Platform/GLFW/GlfwWindow.h"

#include <GLFW/glfw3.h>
#ifdef __EMSCRIPTEN__
#  include <emscripten/emscripten.h>
#endif

namespace Engine {

	Application* Application::s_Instance = nullptr;

	Application::Application()
	{
		CORE_ASSERT(!s_Instance, "Application already exists!");
		s_Instance = this;

		m_Window = std::unique_ptr<Window>(Window::Create());
		m_Window->SetEventCallback(BIND_EVENT_FN(Application::OnEvent));

		// Bring up WebGPU bound to the GLFW window. Static cast is safe —
		// Window::Create only ever returns a GlfwWindow on the platforms we
		// build for.
		auto* glfwWindow = static_cast<GlfwWindow*>(m_Window.get())->GetNativeHandle();
		const bool ok = m_Gfx.Init(glfwWindow,
		                           m_Window->GetWidth(),
		                           m_Window->GetHeight());
		CORE_ASSERT(ok, "WGPUContext::Init failed");

		Renderer::Init(&m_Gfx);
	}

	Application::~Application()
	{
		Renderer::Shutdown();
		m_Gfx.Shutdown();
	}

	void Application::PushLayer(Layer* layer)    { m_LayerStack.PushLayer(layer); }
	void Application::PushOverlay(Layer* layer)  { m_LayerStack.PushOverlay(layer); }

	void Application::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_EVENT_FN(Application::OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FN(Application::OnWindowResize));

		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin(); )
		{
			(*--it)->OnEvent(e);
			if (e.Handled) break;
		}
	}

	void Application::RunOneFrame()
	{
		float time = (float)glfwGetTime();
		Timestep timestep = time - m_LastFrameTime;
		m_LastFrameTime = time;

		for (Layer* layer : m_LayerStack)
			layer->OnUpdate(timestep);

		Input::OnUpdate();
		m_Window->OnUpdate();
	}

#ifdef __EMSCRIPTEN__
	static void EmscriptenMainLoop(void* arg)
	{
		Application* app = static_cast<Application*>(arg);
		app->RunOneFrame();
	}
#endif

	void Application::Run()
	{
		Input::Init();
#ifdef __EMSCRIPTEN__
		emscripten_set_main_loop_arg(EmscriptenMainLoop, this, 0, 1);
#else
		while (m_Running)
			RunOneFrame();
#endif
	}

	bool Application::OnWindowClose(WindowCloseEvent&)
	{
		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		m_Gfx.OnResize(e.GetWidth(), e.GetHeight());
		return false;
	}

}
