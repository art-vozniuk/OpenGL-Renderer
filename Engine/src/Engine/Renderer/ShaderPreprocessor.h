#pragma once

#include <string>

namespace Engine {

	/*
	 * ShaderPreprocessor
	 * ------------------
	 * Turns a platform-agnostic GLSL source into a full, compilable shader:
	 *   1. Prepends a #version + (on GLES) precision qualifier header
	 *      so individual .glsl files don't need to repeat those lines.
	 *   2. Resolves  #include "path/to/snippet.glsl"  directives recursively
	 *      relative to a configurable base directory, with a seen-set that
	 *      behaves like #pragma once.
	 *   3. Emits #line directives so GLSL compile errors point at the real
	 *      file + line number even after inlining.
	 *
	 * The class is intentionally standalone — it has no dependency on any
	 * Shader/FileReader type so it can be unit-tested or reused.
	 */
	class ShaderPreprocessor
	{
	public:
		enum class Stage
		{
			Vertex,
			Fragment,
		};

		struct Options
		{
			// Directory that #include "foo" paths are resolved against.
			// Typically ${assets}/shaders.
			std::string IncludeBaseDir;

			// Max depth of recursive #include traversal. A safety guard
			// for accidental cycles; seen-set normally prevents them.
			int MaxIncludeDepth = 16;

			// Injects platform-appropriate #version line and precision
			// qualifiers (for GLES fragment). Set to false only if you
			// want to emit a header yourself.
			bool InjectPlatformHeader = true;
		};

		// Runs the full preprocessing pipeline. On fatal errors (missing
		// include, too-deep nesting) logs a warning and inserts an #error
		// line so the downstream GL compiler fails loudly.
		static std::string Process(const std::string& source,
		                           Stage stage,
		                           const Options& options);

		// Exposed for tests: builds just the platform header (no includes).
		static std::string BuildPlatformHeader(Stage stage);
	};

}
