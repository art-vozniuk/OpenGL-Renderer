#pragma once

#include "Core.h"
#include "spdlog/spdlog.h"
// std::format used directly by spdlog (SPDLOG_USE_STD_FORMAT)

namespace Engine {

	class ENGINE_API Log
	{
	public:
		static void Init();

		inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
	private:
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
	};

}

// Core log macros
#define TRACE_CORE(...)    ::Engine::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define INFO_CORE(...)     ::Engine::Log::GetCoreLogger()->info(__VA_ARGS__)
#define WARN_CORE(...)     ::Engine::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define ERROR_CORE(...)    ::Engine::Log::GetCoreLogger()->error(__VA_ARGS__)
#define CRITICAL_CORE(...) ::Engine::Log::GetCoreLogger()->critical(__VA_ARGS__)

// Client log macros
#define TRACE_CL(...)         ::Engine::Log::GetClientLogger()->trace(__VA_ARGS__)
#define INFO_CL(...)          ::Engine::Log::GetClientLogger()->info(__VA_ARGS__)
#define WARN_CL(...)          ::Engine::Log::GetClientLogger()->warn(__VA_ARGS__)
#define ERROR_CL(...)         ::Engine::Log::GetClientLogger()->error(__VA_ARGS__)
#define CRITICAL_CL(...)      ::Engine::Log::GetClientLogger()->critical(__VA_ARGS__)
