// ImGui layer is disabled on the WebGPU branch -- the upstream backend
// imgui_impl_opengl3 won't link without GL, and imgui_impl_wgpu needs
// matched-version headers that we haven't vendored yet. Re-enable as a
// follow-up once the WebGPU rendering path is settled.

#include "pch.h"
#include "ImGuiLayer.h"

namespace Engine {

	ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer (disabled)") {}

	void ImGuiLayer::OnAttach()       {}
	void ImGuiLayer::OnDetach()       {}
	void ImGuiLayer::OnImGuiRender()  {}
	void ImGuiLayer::Begin()          {}
	void ImGuiLayer::End()            {}

}
