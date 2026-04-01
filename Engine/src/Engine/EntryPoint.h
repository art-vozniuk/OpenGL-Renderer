#pragma once

extern Engine::Application* Engine::CreateApplication();

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	Engine::Log::Init();

	auto app = Engine::CreateApplication();
	app->Run();
	delete app;
}
