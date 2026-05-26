#pragma once

#include "SceneBase.h"

#include "Engine/Renderer/MeshRenderer.h"

#include <memory>

namespace Sandbox {

	/*
	 * GlbViewerScene — single-GLB orbital viewer.
	 *
	 * Fetches a .glb from ?scene_url=, parses it via GltfLoader, uploads
	 * to one MeshRenderer, and installs an orbit camera framed on the
	 * mesh's AABB. No grid, no gizmos, no editor chrome — just the mesh
	 * + orbit/fly. Used by the TRELLIS pipeline viewer.
	 */
	class GlbViewerScene final : public SceneBase
	{
	public:
		GlbViewerScene(float screenWidth, float screenHeight);
		~GlbViewerScene() override;

		void OnUpdate(Engine::Timestep ts) override;

	private:
		void EnsureDepthTexture(uint32_t w, uint32_t h);

		std::unique_ptr<Engine::MeshRenderer> m_Mesh;
		bool m_PrevTabDown = false;

		WGPUTexture     m_DepthTex   = nullptr;
		WGPUTextureView m_DepthView  = nullptr;
		uint32_t        m_DepthWidth  = 0;
		uint32_t        m_DepthHeight = 0;
	};

}
