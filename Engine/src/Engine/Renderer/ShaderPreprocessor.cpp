#include "pch.h"
#include "ShaderPreprocessor.h"

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <filesystem>

namespace Engine {

	namespace fs = std::filesystem;

	namespace {

		// Trim ASCII whitespace from both ends, in place.
		void TrimInPlace(std::string& s)
		{
			auto notSpace = [](unsigned char c) { return !std::isspace(c); };
			s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
			s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
		}

		// If the line is  #include "foo/bar.glsl"  (whitespace flexible),
		// returns "foo/bar.glsl". Otherwise empty.
		std::string ParseIncludeDirective(const std::string& line)
		{
			std::string t = line;
			TrimInPlace(t);
			static const std::string kDirective = "#include";
			if (t.size() < kDirective.size() + 3) return {};
			if (t.compare(0, kDirective.size(), kDirective) != 0) return {};

			// After "#include", must be whitespace then a quoted string.
			size_t i = kDirective.size();
			while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
			if (i >= t.size() || t[i] != '"') return {};
			size_t start = i + 1;
			size_t end = t.find('"', start);
			if (end == std::string::npos) return {};
			return t.substr(start, end - start);
		}

		// Read a whole file as a string. On failure returns nullopt-equivalent
		// via the outSuccess flag; keeps the interface simple.
		bool TryReadFile(const fs::path& path, std::string& out)
		{
			std::ifstream stream(path);
			if (!stream) return false;
			std::ostringstream ss;
			ss << stream.rdbuf();
			out = ss.str();
			return true;
		}

		// Recursive #include resolver. `seen` implements #pragma once.
		// `fileId` is a small integer used in #line directives (the GLSL
		// spec only allows integer file numbers, not file names).
		void ExpandIncludes(const std::string& source,
		                    const fs::path& baseDir,
		                    int depth,
		                    int maxDepth,
		                    std::unordered_set<std::string>& seen,
		                    int& nextFileId,
		                    int currentFileId,
		                    std::ostringstream& out)
		{
			std::istringstream in(source);
			std::string line;
			int lineNo = 0;

			while (std::getline(in, line))
			{
				++lineNo;
				std::string incPath = ParseIncludeDirective(line);

				if (incPath.empty())
				{
					out << line << '\n';
					continue;
				}

				if (depth >= maxDepth)
				{
					WARN_CORE("ShaderPreprocessor: include depth limit ({0}) exceeded at {1}", maxDepth, incPath);
					out << "#error \"include depth exceeded: " << incPath << "\"\n";
					continue;
				}

				// #pragma once semantics.
				auto canonical = (baseDir / incPath).lexically_normal().string();
				if (!seen.insert(canonical).second)
				{
					// Already included; emit nothing but keep line numbering accurate.
					out << "// (skipped duplicate include: " << incPath << ")\n";
					continue;
				}

				std::string included;
				if (!TryReadFile(canonical, included))
				{
					WARN_CORE("ShaderPreprocessor: failed to open include {0}", canonical);
					out << "#error \"include not found: " << incPath << "\"\n";
					continue;
				}

				int includedFileId = nextFileId++;
				out << "// >>> " << incPath << "\n";
				out << "#line 1 " << includedFileId << "\n";

				ExpandIncludes(included,
				               fs::path(canonical).parent_path(),
				               depth + 1,
				               maxDepth,
				               seen,
				               nextFileId,
				               includedFileId,
				               out);

				out << "// <<< " << incPath << "\n";
				// Restore current file's line tracking for subsequent compile errors.
				out << "#line " << (lineNo + 1) << " " << currentFileId << "\n";
			}
		}

	} // namespace


	std::string ShaderPreprocessor::BuildPlatformHeader(Stage stage)
	{
		std::string out;
#ifdef __EMSCRIPTEN__
		out += "#version 300 es\n";
		if (stage == Stage::Fragment)
		{
			out += "precision highp float;\n";
			out += "precision highp int;\n";
			out += "precision highp sampler2D;\n";
			out += "precision highp samplerCube;\n";
		}
#else
		out += "#version 330 core\n";
		(void)stage;
#endif
		return out;
	}


	std::string ShaderPreprocessor::Process(const std::string& source,
	                                        Stage stage,
	                                        const Options& options)
	{
		std::ostringstream out;

		if (options.InjectPlatformHeader)
		{
			out << BuildPlatformHeader(stage);
		}

		// The main (non-included) source is file id 0. Each #include gets a
		// fresh id so GL error messages like "0(12): ..." vs "3(5): ..." can
		// be disambiguated against log output.
		std::unordered_set<std::string> seen;
		int nextFileId = 1;
		out << "#line 1 0\n";
		ExpandIncludes(source,
		               fs::path(options.IncludeBaseDir),
		               /*depth=*/0,
		               options.MaxIncludeDepth,
		               seen,
		               nextFileId,
		               /*currentFileId=*/0,
		               out);

		return out.str();
	}

}
