#include "SceneRegistry.h"

namespace Sandbox {

	SceneRegistry& SceneRegistry::Instance()
	{
		static SceneRegistry s_instance;
		return s_instance;
	}


	void SceneRegistry::Register(const std::string& id, Factory factory)
	{
		auto [it, inserted] = m_Factories.insert({ id, std::move(factory) });
		if (inserted) {
			m_Ids.push_back(id);
		} else {
			it->second = std::move(factory); // overwrite, don't re-list
		}
	}


	SceneBase* SceneRegistry::Create(const std::string& id, float screenWidth, float screenHeight) const
	{
		auto it = m_Factories.find(id);
		if (it == m_Factories.end()) return nullptr;
		return it->second(screenWidth, screenHeight);
	}

}
