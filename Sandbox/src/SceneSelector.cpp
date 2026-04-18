#include "SceneSelector.h"

#include <vector>
#include <cstring>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Sandbox {

	namespace {

		// argv captured from main(). Native only. On web this stays empty.
		std::vector<std::string> s_Argv;

		// Basic sanity check: allow a-z, 0-9, '-', '_' so we never feed weird
		// user input into AssetManager path concatenation.
		bool IsValidId(const std::string& s)
		{
			if (s.empty() || s.size() > 64) return false;
			for (char c : s) {
				bool ok = (c >= 'a' && c <= 'z')
				       || (c >= '0' && c <= '9')
				       || c == '-' || c == '_';
				if (!ok) return false;
			}
			return true;
		}

		std::string ParseArgvScene()
		{
			constexpr const char* kPrefix = "--scene=";
			const size_t kPrefixLen = std::strlen(kPrefix);
			for (const auto& a : s_Argv) {
				if (a.size() > kPrefixLen && a.compare(0, kPrefixLen, kPrefix) == 0) {
					return a.substr(kPrefixLen);
				}
			}
			return {};
		}

	#ifdef __EMSCRIPTEN__
		// Reads `?scene=<id>` from the browser's window.location.search.
		// EM_ASM_PTR hands back a malloc'd C-string which we free here.
		std::string ReadQueryScene()
		{
			char* raw = (char*)EM_ASM_PTR({
				try {
					var params = new URLSearchParams(window.location.search || "");
					var v = params.get('scene');
					if (!v) return 0;
					var lengthBytes = lengthBytesUTF8(v) + 1;
					var ptr = _malloc(lengthBytes);
					stringToUTF8(v, ptr, lengthBytes);
					return ptr;
				} catch (e) {
					return 0;
				}
			});
			if (!raw) return {};
			std::string out(raw);
			std::free(raw);
			return out;
		}
	#endif

	} // namespace


	void SetCommandLine(int argc, char** argv)
	{
		s_Argv.clear();
		s_Argv.reserve(argc);
		for (int i = 0; i < argc; ++i) {
			if (argv[i]) s_Argv.emplace_back(argv[i]);
		}
	}


	std::string SelectScene(const std::string& defaultId)
	{
	#ifdef __EMSCRIPTEN__
		auto web = ReadQueryScene();
		if (IsValidId(web)) return web;
	#endif
		auto native = ParseArgvScene();
		if (IsValidId(native)) return native;
		return defaultId;
	}

}
