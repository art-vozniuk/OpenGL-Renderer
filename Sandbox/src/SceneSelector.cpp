#include "SceneSelector.h"

#include <vector>
#include <cstring>
#include <cstdlib>
#include <sstream>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Sandbox {

	namespace {

		// argv captured from main(). Native only. On web this stays empty.
		std::vector<std::string> s_Argv;

		// Basic sanity check for scene IDs: allow a-z, 0-9, '-', '_' so we
		// never feed weird user input into AssetManager path concatenation.
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

		// argv lookup for --<key>=<value>. Returns the value (possibly empty)
		// if found, std::nullopt otherwise.
		std::optional<std::string> ParseArgvParam(const char* key)
		{
			std::string prefix = "--";
			prefix += key;
			prefix += '=';
			for (const auto& a : s_Argv) {
				if (a.size() >= prefix.size()
				    && a.compare(0, prefix.size(), prefix) == 0) {
					return a.substr(prefix.size());
				}
			}
			return std::nullopt;
		}

	#ifdef __EMSCRIPTEN__
		// Reads `?<key>=<value>` from the browser's window.location.search.
		// EM_ASM_PTR hands back a malloc'd C-string which we free here.
		std::optional<std::string> ParseQueryParam(const char* key)
		{
			char* raw = (char*)EM_ASM_PTR({
				try {
					var key = UTF8ToString($0);
					var params = new URLSearchParams(window.location.search || "");
					var v = params.get(key);
					if (v === null) return 0;
					var lengthBytes = lengthBytesUTF8(v) + 1;
					var ptr = _malloc(lengthBytes);
					stringToUTF8(v, ptr, lengthBytes);
					return ptr;
				} catch (e) {
					return 0;
				}
			}, key);
			if (!raw) return std::nullopt;
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


	std::optional<std::string> ReadParam(const char* key)
	{
	#ifdef __EMSCRIPTEN__
		auto web = ParseQueryParam(key);
		if (web && !web->empty()) return web;
	#endif
		auto native = ParseArgvParam(key);
		if (native && !native->empty()) return native;
		return std::nullopt;
	}


	std::optional<glm::vec3> ParseVec3(const std::string& s)
	{
		// Accept "x,y,z" (with optional surrounding spaces around each
		// comma-separated component).
		float v[3] = {0.f, 0.f, 0.f};
		size_t comp = 0;
		std::stringstream ss(s);
		std::string tok;
		while (std::getline(ss, tok, ',')) {
			if (comp >= 3) return std::nullopt;
			try {
				v[comp++] = std::stof(tok);
			} catch (...) {
				return std::nullopt;
			}
		}
		if (comp != 3) return std::nullopt;
		return glm::vec3(v[0], v[1], v[2]);
	}


	std::string SelectScene(const std::string& defaultId)
	{
		auto p = ReadParam("scene");
		if (p && IsValidId(*p)) return *p;
		return defaultId;
	}

}
