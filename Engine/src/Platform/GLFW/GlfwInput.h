#pragma once

#include "Engine/Input.h"
#include "GLFW/glfw3.h"

namespace Engine {

	class GlfwInput : public Input
	{
	public:
		Position m_ScrollState;

	protected:
		virtual bool IsKeyPressedImpl(int keycode) override;
		virtual bool IsMouseButtonPressedImpl(int button) override;
		virtual Position GetMousePositionImpl(void) override;
		virtual float GetMouseXImpl(void) override;
		virtual float GetMouseYImpl(void) override;
		virtual Position GetScrollImpl(void) override;
		virtual void OnUpdateImpl(void) override;
		virtual void InitImpl(void) override;
	};

}
