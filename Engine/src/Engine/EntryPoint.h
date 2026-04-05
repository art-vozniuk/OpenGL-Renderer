#pragma once

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

extern Engine::Application* Engine::CreateApplication();

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	Engine::Log::Init();

	auto app = Engine::CreateApplication();
	app->Run();
	// Note: under Emscripten, Run() never returns (simulate_infinite_loop=1),
	// so delete is effectively unreachable — but kept for native correctness.
	delete app;
}
