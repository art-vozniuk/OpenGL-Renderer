#include "pch.h"
#include "GlfwInput.h"

#include "Engine/Application.h"
#include <GLFW/glfw3.h>

namespace Engine {

	Input* Input::s_Instance = new GlfwInput();

	static GlfwInput& GetGlfwInstance(void)
	{
		return *static_cast<GlfwInput*>(Input::GetInstance());
	}

	static void ScrollCB(GLFWwindow* window, double x, double y)
	{
		GetGlfwInstance().m_ScrollState = { x, y };
	}

	void GlfwInput::InitImpl(void)
	{
		auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		glfwSetScrollCallback(window, ScrollCB);
	}

	bool GlfwInput::IsKeyPressedImpl(int keycode)
	{
		auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		auto state = glfwGetKey(window, keycode);
		return state == GLFW_PRESS || state == GLFW_REPEAT;
	}

	bool GlfwInput::IsMouseButtonPressedImpl(int button)
	{
		auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		auto state = glfwGetMouseButton(window, button);
		return state == GLFW_PRESS;
	}

	Engine::Input::Position GlfwInput::GetMousePositionImpl(void)
	{
		auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		double xpos, ypos;
		glfwGetCursorPos(window, &xpos, &ypos);

		return { (float)xpos, (float)ypos };
	}

	float GlfwInput::GetMouseXImpl(void)
	{
		auto [x, y] = GetMousePositionImpl();
		return x;
	}

	float GlfwInput::GetMouseYImpl(void)
	{
		auto [x, y] = GetMousePositionImpl();
		return y;
	}

	Engine::Input::Position GlfwInput::GetScrollImpl(void)
	{
		return m_ScrollState;
	}

	void GlfwInput::OnUpdateImpl(void)
	{
		m_ScrollState = { 0.f, 0.f };
	}

}
