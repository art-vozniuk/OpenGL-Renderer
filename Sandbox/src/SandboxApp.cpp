#include <Engine.h>

#include "Scenes/SceneRegistry.h"
#include "SceneSelector.h"

/*
 * Sandbox application entry point.
 *
 * All per-scene setup lives in Sandbox::*Scene classes under Scenes/.
 * The role of this file is just:
 *   1) pick a scene id (CLI arg on native, URL query param on web),
 *   2) ask the registry for an instance of that scene,
 *   3) push it onto the layer stack.
 *
 * Adding a new scene = new class + SCENE_REGISTER macro, nothing here changes.
 */
class SandboxApp : public Engine::Application
{
public:
	SandboxApp()
	{
		const float w = (float)GetWindow().GetWidth();
		const float h = (float)GetWindow().GetHeight();

		auto& registry = Sandbox::SceneRegistry::Instance();
		const std::string requested = Sandbox::SelectScene(registry.DefaultId());

		Sandbox::SceneBase* scene = registry.Create(requested, w, h);
		if (!scene && requested != registry.DefaultId())
		{
			WARN_CORE("Unknown scene id '{0}', falling back to '{1}'", requested, registry.DefaultId());
			scene = registry.Create(registry.DefaultId(), w, h);
		}
		CORE_ASSERT(scene, "No scene registered — build mis-configured?");

		INFO_CORE("Active scene: {0}", scene->Id());
		PushLayer(scene);
	}
};

Engine::Application* Engine::CreateApplication(int argc, char** argv)
{
	// Capture argv so SceneSelector can parse --scene=<id> from native CLI.
	// On web, argv is ignored and the selector reads the URL query instead.
	Sandbox::SetCommandLine(argc, argv);
	return new SandboxApp();
}
