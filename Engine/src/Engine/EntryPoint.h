#pragma once

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

// Client apps define this; argv is forwarded so they can parse flags like
// --scene=<id> without the engine having to know about app-specific options.
extern Engine::Application* Engine::CreateApplication(int argc, char** argv);

int main(int argc, char** argv)
{
	Engine::Log::Init();

	auto app = Engine::CreateApplication(argc, argv);
	app->Run();
	// Note: under Emscripten, Run() never returns (simulate_infinite_loop=1),
	// so delete is effectively unreachable — but kept for native correctness.
	delete app;
}
