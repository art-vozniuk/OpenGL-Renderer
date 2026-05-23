#pragma once

#include "SceneBase.h"

#include "Engine/Core/Transform.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"
#include "Engine/Renderer/GridRenderer.h"
#include "Engine/Renderer/GizmoRenderer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Sandbox {

	/*
	 * EditorScene — 3D editor with multi-object selection and a single
	 * unified gizmo (translate arrows + rotate rings + scale cubes all
	 * visible at once, hit-kind determined by which handle the user
	 * grabs). Fly camera only — no orbit mode here.
	 *
	 * Per-object state (EditorObject) holds an id, display name, splat
	 * renderer, world transform, world-space AABB, and a visibility
	 * flag. New objects are loaded via LoadSplatFromBytes(bytes, name)
	 * and *append* to the scene; nothing is replaced.
	 *
	 * Selection is by object id. Bounding boxes are drawn as corner
	 * brackets — bright for the selected object, dim for the rest, so
	 * users always have a visible target to click.
	 *
	 * JS bridge — see the extern "C" exports in EditorScene.cpp. Most
	 * messages carry an `id` for the object they refer to.
	 */
	class EditorScene final : public SceneBase
	{
	public:
		using ObjectId = uint64_t;

		struct EditorObject
		{
			ObjectId          id   = 0;
			std::string       name;
			Engine::Transform transform;
			bool              visible = true;
			// Object-space coordinates of the pivot (bbox bottom-center).
			// Renderer applies T(-pivotObj) before T*R*S so transform.position
			// directly equals the pivot's world position.
			glm::vec3         pivotObj{0.0f};
			std::unique_ptr<Engine::GaussianSplatRenderer> splat;
		};

		EditorScene(float screenWidth, float screenHeight);
		~EditorScene() override;

		void OnUpdate(Engine::Timestep ts) override;

		// --- JS bridge entry points ------------------------------------------
		// Load a .splat blob as a NEW object (does not replace existing).
		// `name` is the display name; pass empty to auto-generate.
		ObjectId LoadSplatFromBytes(const uint8_t* data, size_t size, const std::string& name);
		void     DeleteObject(ObjectId id);
		void     SelectObject(ObjectId id); // 0 = deselect
		void     FocusObject(ObjectId id);
		void     RenameObject(ObjectId id, const std::string& name);
		void     SetVisibility(ObjectId id, bool visible);
		// Apply a whole-transform update from the React inspector. Rotation
		// is euler degrees (XYZ order), matching what PostTransformUpdate emits.
		void     SetTransform(ObjectId id, const glm::vec3& pos,
		                      const glm::vec3& eulerDeg, const glm::vec3& scale);
		void     SetSnap(bool snap);
		// Teleport the fly camera to (pos, looking along fwd). React calls
		// this from the manifest hydrate path and from inspector edits.
		void     SetCameraPose(const glm::vec3& pos, const glm::vec3& fwd);
		void     ClearAll();

		static EditorScene* Current() { return s_Current; }

	private:
		// ---- Gizmo hit testing ---------------------------------------------
		struct GizmoHit
		{
			enum class Kind { None, TranslateAxis, RotateRing, ScaleAxis, ScaleUniform };
			Kind  kind = Kind::None;
			int   axis = -1;
			float distancePx = 1e9f;
		};

		struct DragState
		{
			bool             active = false;
			GizmoHit::Kind   kind   = GizmoHit::Kind::None;
			int              axis   = -1;
			ObjectId         objectId = 0;
			glm::vec2        startCursor{0.0f};
			Engine::Transform startTransform{};
			// Pivot captured at LMB press; drag math anchors here for the whole gesture.
			glm::vec3        dragPivot{0.0f};
			float            startProjectionT = 0.0f; // translate
			float            startAngleRad    = 0.0f; // rotate
			float            currentAngleRad  = 0.0f; // rotate — for arc rendering
			float            startDistPx      = 0.0f; // scale
		};

		// --- Per-frame routines ---------------------------------------------
		void HandleHotkeys();
		void HandleMouseInteraction(const glm::vec2& fbViewport);

		void BuildSceneGizmos(const glm::vec2& viewport);   // builds bbox + gizmo lines

		// --- Object management ----------------------------------------------
		std::string AutoNameFor(const std::string& filenameStem) const;
		EditorObject* FindObject(ObjectId id);
		const EditorObject* FindObject(ObjectId id) const;
		EditorObject* SelectedObject();

		// World-space AABB of `o` (object-space AABB transformed by its model
		// matrix and refit to axis-aligned).
		void ComputeWorldAabb(const EditorObject& o, glm::vec3& outMin, glm::vec3& outMax) const;
		// Visual gizmo pivot: center of the selected object's world AABB.
		glm::vec3 GizmoPivot() const;

		static glm::vec3 AxisDir(int axis);
		float            GizmoWorldScale(const Engine::SPtr<Engine::Camera>& cam,
		                                 const glm::vec3& pivot) const;

		bool RaycastObject(const EditorObject& o, const glm::vec3& origin,
		                   const glm::vec3& dir, float& outT) const;
		ObjectId PickObject(const glm::vec3& origin, const glm::vec3& dir) const;
		GizmoHit PickGizmoHandle(const glm::vec2& cursor,
		                         const glm::vec2& viewport) const;

		void BeginDrag(const GizmoHit& hit, const glm::vec2& cursor,
		               const glm::vec2& viewport);
		void UpdateDrag(const glm::vec2& cursor, const glm::vec2& viewport);
		void EndDrag();

		// --- Posting --------------------------------------------------------
		void PostObjectsList();
		void PostSelectionChanged();
		void PostTransformUpdate(bool isFinal);

		// --- Scene state -----------------------------------------------------
		std::vector<EditorObject>             m_Objects;
		ObjectId                              m_NextId    = 1;
		ObjectId                              m_Selected  = 0;
		bool                                  m_SnapToggle = false;
		GizmoHit                              m_Hover;
		DragState                             m_Drag;

		// --- Renderers --------------------------------------------------------
		std::unique_ptr<Engine::GridRenderer>  m_Grid;
		std::unique_ptr<Engine::GizmoRenderer> m_Gizmo;

		// --- Mouse/key edge bookkeeping ---------------------------------------
		bool      m_PrevLmb           = false;
		glm::vec2 m_LmbPressCursor{0.0f};

		static EditorScene* s_Current;
	};

}
