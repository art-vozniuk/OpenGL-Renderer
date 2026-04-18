#pragma once

#include "pch.h"
#include "../Scene.h"

namespace Engine {

	// Loads a glTF 2.0 file (binary .glb or text .gltf) using tinygltf.
	// Returns a populated Model on success, nullptr on failure.
	class GltfLoader
	{
	public:
		static SPtr<Scn::Model> Load(const std::string& path);
	};

} // namespace Engine
