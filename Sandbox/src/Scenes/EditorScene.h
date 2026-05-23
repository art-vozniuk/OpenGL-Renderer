#pragma once

#include "SceneBase.h"

#include "Engine/Core/Transform.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"
#include "Engine/Renderer/GridRenderer.h"
#include "Engine/Renderer/GizmoRenderer.h"

#include <memory>
#include <string>

namespace Sandbox {

	/*
	 * EditorScene
	 *
	 * 3D editor scene: grid floor, optional Gaussian-splat content, full
	 * transform gizmos (translate / rotate / scale) on the selected
	 * object. Mouse:
	 *   - RMB drag       → camera (fly or orbit, see CameraMode)
	 *   - LMB click      → select object under cursor (or deselect)
	 *   - LMB drag on a gizmo axis → modify the selected transform
	 * Keys: W/E/R switch tool, Tab returns camera to orbit, Ctrl = snap.
	 *
	 * Each tool, mode change, drag start/update/end, and selection
	 * change is posted to the parent frame so the React UI can render
	 * a numeric HUD + inspector. See EditorScene.cpp for the message
	 * schema.
	 */
	class EditorScene final : public SceneBase
	{
	public:
		EditorScene(float screenWidth, float screenHeight);
		~EditorScene() override;

		void OnUpdate(Engine::Timestep ts) override;

		// JS bridge entries (see C wrappers at bottom of EditorScene.cpp).
		void LoadSplatFromBytes(const uint8_t* data, size_t size);
		void ClearScene();
		void SetTool(int tool);     // 0=translate, 1=rotate, 2=scale
		void SetSnap(bool snap);    // snap on/off (overrides Ctrl modifier)

		static EditorScene* Current() { return s_Current; }

	private:
		enum class Tool { Translate = 0, Rotate = 1, Scale = 2 };

		// Which gizmo handle the user is currently hovering / dragging.
		// 0/1/2 = X/Y/Z axis; 3 = uniform (center for scale, screen-axis
		// for rotate); -1 = none.
		struct GizmoHit
		{
			int axis = -1;
			float distancePx = 1e9f;
		};

		// Drag state captured at LMB press, used to compute the delta
		// against the current cursor pos every frame.
		struct DragState
		{
			bool             active = false;
			Tool             tool   = Tool::Translate;
			int              axis   = -1;
			glm::vec2        startCursor{0.0f};
			Engine::Transform startTransform{};
			float            startProjectionT = 0.0f; // axis t at press (translate)
			float            startAngleRad    = 0.0f; // initial angle (rotate)
			float            startDistPx      = 0.0f; // pivot→cursor px (scale)
		};

		// --- Per-frame routines ----------------------------------------------
		void HandleHotkeys();
		void HandleMouseInteraction(const glm::vec2& viewport);

		void BuildGizmoPrimitives(const glm::vec2& viewport);

		// --- Geometry helpers ------------------------------------------------
		// Pivot in world space = transform.position (gizmo origin).
		glm::vec3 GizmoPivot() const { return m_Transform.position; }

		// World-space basis vectors of the current gizmo (always world for
		// now — local-space toggle is a future iteration).
		static glm::vec3 AxisDir(int axis);

		// Constant pixel-size gizmo: scale world-space length so the gizmo
		// takes a fixed fraction of the screen regardless of distance.
		float GizmoWorldScale(const Engine::SPtr<Engine::Camera>& cam) const;

		// Ray vs object's loose world-space AABB (transformed by current
		// model matrix). Returns true if hit.
		bool RaycastSplat(const glm::vec3& origin, const glm::vec3& dir) const;

		// Try each gizmo handle in screen space; return the closest within
		// the pixel threshold, or { -1, huge } if nothing close enough.
		GizmoHit PickGizmoHandle(const glm::vec2& cursor,
		                         const glm::vec2& viewport) const;

		// --- Drag math -------------------------------------------------------
		void BeginDrag(int axis, const glm::vec2& cursor, const glm::vec2& viewport);
		void UpdateDrag(const glm::vec2& cursor, const glm::vec2& viewport);
		void EndDrag();

		// --- Posting --------------------------------------------------------
		void PostTransformUpdate(bool isFinal);
		void PostToolChanged();
		void PostSelectionChanged();

		std::unique_ptr<Engine::GridRenderer>          m_Grid;
		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;
		std::unique_ptr<Engine::GizmoRenderer>         m_Gizmo;
		size_t                                         m_SplatCount = 0;

		Engine::Transform m_Transform;
		bool              m_HasContent = false;
		bool              m_Selected   = false;

		Tool      m_Tool       = Tool::Translate;
		bool      m_SnapToggle = false; // toolbar toggle; Ctrl also enables
		GizmoHit  m_Hover;
		DragState m_Drag;

		// Edge-detection bookkeeping for mouse / keys.
		bool      m_PrevLmb    = false;
		bool      m_PrevW      = false;
		bool      m_PrevE      = false;
		bool      m_PrevR      = false;
		bool      m_PrevTab    = false;
		glm::vec2 m_LmbPressCursor{0.0f};
		bool      m_LmbPressFromGizmo = false;

		double m_PrevFrameStart = 0.0;
		int    m_FpsCounter     = 0;
		double m_FpsT0          = 0.0;

		static EditorScene* s_Current;
	};

}
