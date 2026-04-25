#pragma once

#include "Engine/Layer.h"

namespace Sandbox {

	/*
	 * SceneBase
	 *
	 * Thin wrapper over Engine::Layer used by all Sandbox scenes:
	 *  - Constructor takes the current framebuffer size so the scene can
	 *    set up its camera + per-frame buffers immediately.
	 *  - Id() is the stable identifier used in the scene registry and
	 *    in URL / CLI selectors (e.g. "gsplat").
	 *
	 * Scenes participate in the normal Layer lifecycle (OnAttach /
	 * OnUpdate / OnEvent / OnDetach).
	 */
	class SceneBase : public Engine::Layer
	{
	public:
		SceneBase(const std::string& id, float screenWidth, float screenHeight)
			: Engine::Layer(id)
			, m_Id(id)
			, m_ScreenWidth(screenWidth)
			, m_ScreenHeight(screenHeight)
		{
		}

		const std::string& Id() const { return m_Id; }

	protected:
		std::string m_Id;
		float m_ScreenWidth = 0.0f;
		float m_ScreenHeight = 0.0f;
	};

}
