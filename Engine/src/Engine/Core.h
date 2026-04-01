#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

// The engine is currently built as a static library on all supported platforms.
#define ENGINE_API

#if defined(_MSC_VER)
#define ENGINE_DEBUGBREAK() __debugbreak()
#elif defined(__clang__)
#define ENGINE_DEBUGBREAK() __builtin_debugtrap()
#elif defined(__GNUC__)
#define ENGINE_DEBUGBREAK() __builtin_trap()
#else
#define ENGINE_DEBUGBREAK() std::abort()
#endif

#if defined(RELEASE) || defined(DEBUG)
#define ENABLE_ASSERTS
#endif

#if defined(_MSC_VER)
#pragma warning (disable : 4353)
#endif

#ifdef ENABLE_ASSERTS
#define ASSERT(x, ...) { if(!(x)) { ERROR_CORE("Assertion Failed: {0}", __VA_ARGS__); ENGINE_DEBUGBREAK(); } }
#define ASSERT_FAIL(...) { ERROR_CORE("Assertion Failed: {0}", __VA_ARGS__); ENGINE_DEBUGBREAK(); }
#define CORE_ASSERT(x, ...) { if(!(x)) { ERROR_CORE("Assertion Failed: {0}", __VA_ARGS__); ENGINE_DEBUGBREAK(); } }
#else
#define ASSERT(x, ...)
#define ASSERT_FAIL(...)
#define CORE_ASSERT(x, ...)
#endif

#define BIT(x) (1 << x)

#define BIND_EVENT_FN(fn) std::bind(&fn, this, std::placeholders::_1)

using uint = uint32_t;
using uint64 = uint64_t;
using int64 = int64_t;


namespace Engine {
	template<typename T>
	using UPtr = std::unique_ptr<T>;

	template<typename T>
	using SPtr = std::shared_ptr<T>;

	template<typename T>
	using WPtr = std::weak_ptr<T>;

	template <class T, class... Args>
	SPtr<T> MakeShared(Args&&... args) {
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

}
