#pragma once

#include "Core.h"

#include "Window.h"
#include "Engine/LayerStack.h"
#include "Engine/Events/Event.h"
#include "Engine/Events/ApplicationEvent.h"

#include "Engine/Core/Timestep.h"
#include "Engine/Renderer/WGPUContext.h"

namespace Engine {

	class Application
	{
	public:
		Application();
		virtual ~Application();

		void Run();
		void RunOneFrame();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);

		inline Window&      GetWindow() { return *m_Window; }
		// WebGPU device / surface / queue access for scenes that need to
		// build their own pipelines + buffers.
		inline WGPUContext& GetGfx()    { return m_Gfx; }

		inline static Application& Get() { return *s_Instance; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
	private:
		std::unique_ptr<Window> m_Window;
		WGPUContext             m_Gfx;
		bool                    m_Running        = true;
		LayerStack              m_LayerStack;
		float                   m_LastFrameTime  = 0.0f;
	private:
		static Application* s_Instance;
	};

	// Implemented by the client application. Argv passes through from main()
	// for command-line parsing.
	Application* CreateApplication(int argc, char** argv);

}
