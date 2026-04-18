#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "SceneBase.h"

namespace Sandbox {

	/*
	 * SceneRegistry
	 * -------------
	 * Factory for the Sandbox's scene classes. Scenes register a factory
	 * under a stable id at static-init time; the application then asks for
	 * a scene by id (coming from --scene=... on native, ?scene=... on web).
	 *
	 * Keeping this in Sandbox/ (not Engine/) because the list of concrete
	 * scene classes is app-specific — the engine itself stays scene-agnostic.
	 */
	class SceneRegistry
	{
	public:
		using Factory = std::function<SceneBase* (float screenWidth, float screenHeight)>;

		static SceneRegistry& Instance();

		// Registers a factory under `id`. Overwrites any previous factory
		// with the same id (last writer wins — not expected in practice).
		void Register(const std::string& id, Factory factory);

		// Creates a scene by id. Returns nullptr if no matching factory.
		SceneBase* Create(const std::string& id, float screenWidth, float screenHeight) const;

		// Returns the preferred default (currently "sponza"), used as fallback
		// when no id is provided or when the requested id is unknown.
		std::string DefaultId() const { return m_DefaultId; }
		void SetDefaultId(const std::string& id) { m_DefaultId = id; }

		// Enumerates registered ids in insertion order (for logging).
		const std::vector<std::string>& Ids() const { return m_Ids; }

	private:
		SceneRegistry() = default;

		std::unordered_map<std::string, Factory> m_Factories;
		std::vector<std::string> m_Ids;
		std::string m_DefaultId = "gsplat";
	};


}

/*
 * SCENE_REGISTER
 * --------------
 * Helper macro for static-time registration. Invoke *inside* the Sandbox
 * namespace (so the unqualified class name works with ## token-pasting) and
 * pass the scene class + its stable id.
 * Example:
 *     namespace Sandbox {
 *         // class definition...
 *         SCENE_REGISTER("sponza", SponzaScene)
 *     }
 */
#define SCENE_REGISTER(id_str, SceneClass)                                            \
	namespace {                                                                       \
		struct SceneClass##_Registrar {                                               \
			SceneClass##_Registrar() {                                                \
				::Sandbox::SceneRegistry::Instance().Register(id_str,                 \
					[](float w, float h) -> ::Sandbox::SceneBase* {                   \
						return new SceneClass(w, h);                                  \
					});                                                               \
			}                                                                         \
		};                                                                            \
		static SceneClass##_Registrar s_##SceneClass##_registrar;                     \
	}
