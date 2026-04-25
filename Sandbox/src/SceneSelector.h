#pragma once

#include <string>
#include <optional>
#include <glm/glm.hpp>

namespace Sandbox {

	/*
	 * SelectScene
	 * -----------
	 * Resolves which scene id the app should run:
	 *   - Native:   parses argv for --scene=<id> (captured via SetCommandLine).
	 *   - Web:      reads ?scene=<id> from window.location.search via EM_ASM.
	 *
	 * Returns `defaultId` if nothing valid is provided.
	 */
	std::string SelectScene(const std::string& defaultId);

	/*
	 * SetCommandLine
	 * --------------
	 * Called once from main() so that SelectScene() can inspect argv on
	 * native builds without having to be threaded through the whole engine.
	 */
	void SetCommandLine(int argc, char** argv);


	/*
	 * Generic query / argv lookup. On web reads `?<key>=<value>` from
	 * window.location.search; on native looks for `--<key>=<value>` in argv.
	 * Returns std::nullopt if not present (or empty).
	 *
	 * Used by scenes to read runtime parameters (scene_url, scene_path, eye,
	 * fwd, etc.) without hardcoding a query-vs-argv branch in every caller.
	 */
	std::optional<std::string> ReadParam(const char* key);


	/*
	 * Convenience: parse "x,y,z" into a vec3. Returns std::nullopt on any
	 * formatting error (wrong arity, non-numeric, etc.).
	 */
	std::optional<glm::vec3> ParseVec3(const std::string& s);

}
