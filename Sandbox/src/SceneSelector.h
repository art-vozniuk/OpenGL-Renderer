#pragma once

#include <string>

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

}
