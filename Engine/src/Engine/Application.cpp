#include "pch.h"
#include "Application.h"

#include "Engine/Log.h"

#include "Engine/Renderer/Renderer.h"

#include "Input.h"

#ifndef __EMSCRIPTEN__
#include <GLFW/glfw3.h>
#else
#include <GLFW/glfw3.h>
#include <emscripten/emscripten.h>
#endif

namespace Engine {

	Application* Application::s_Instance = nullptr;

	Application::Application()
	{
		CORE_ASSERT(!s_Instance, "Application already exists!");
		s_Instance = this;

		m_Window = std::unique_ptr<Window>(Window::Create());
		m_Window->SetEventCallback(BIND_EVENT_FN(Application::OnEvent));

		Renderer::Init();

		m_ImGuiLayer = new ImGuiLayer();
		PushOverlay(m_ImGuiLayer);
	}

	void Application::PushLayer(Layer* layer)
	{
		m_LayerStack.PushLayer(layer);
	}

	void Application::PushOverlay(Layer* layer)
	{
		m_LayerStack.PushOverlay(layer);
	}

	void Application::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_EVENT_FN(Application::OnWindowClose));

		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin(); )
		{
			(*--it)->OnEvent(e);
			if (e.Handled)
				break;
		}
	}

	void Application::RunOneFrame()
	{
		float time = (float)glfwGetTime();
		Timestep timestep = time - m_LastFrameTime;
		m_LastFrameTime = time;

		for (Layer* layer : m_LayerStack)
			layer->OnUpdate(timestep);

		m_ImGuiLayer->Begin();
		for (Layer* layer : m_LayerStack)
			layer->OnImGuiRender();
		m_ImGuiLayer->End();

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
		// Hand control to the browser — 0 fps means use requestAnimationFrame,
		// simulate_infinite_loop=1 so main() doesn't return prematurely.
		emscripten_set_main_loop_arg(EmscriptenMainLoop, this, 0, 1);
#else
		while (m_Running)
			RunOneFrame();
#endif
	}

	bool Application::OnWindowClose(WindowCloseEvent& e)
	{
		m_Running = false;
		return true;
	}

}
