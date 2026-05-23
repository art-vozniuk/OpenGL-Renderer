#pragma once

#include "SceneBase.h"

#include "Engine/Core/Transform.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"
#include "Engine/Renderer/GridRenderer.h"
#include "Engine/Renderer/GizmoRenderer.h"
#include "Engine/Renderer/MeshRenderer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Sandbox {

	/*
	 * EditorScene — multi-object 3D editor. Objects are polymorphic:
	 * splat, mesh, or directional light. All three are selectable and
	 * transformable; meshes render through a PBR pipeline against the
	 * scene's single directional light + ambient term.
	 */
	class EditorScene final : public SceneBase
	{
	public:
		using ObjectId = uint64_t;

		enum class Kind { Splat, Mesh, Light };

		struct LightProps
		{
			glm::vec3 color     = glm::vec3(1.0f, 0.95f, 0.9f);
			float     intensity = 3.0f;
		};

		struct EditorObject
		{
			ObjectId          id   = 0;
			std::string       name;
			Kind              kind = Kind::Splat;
			Engine::Transform transform;
			bool              visible = true;
			// Object-space coordinates of the pivot (bbox bottom-center).
			// Renderer applies T(-pivotObj) before T*R*S so transform.position
			// directly equals the pivot's world position.
			glm::vec3         pivotObj{0.0f};
			std::unique_ptr<Engine::GaussianSplatRenderer> splat;
			std::unique_ptr<Engine::MeshRenderer>          mesh;
			std::optional<LightProps>                      light;
		};

		EditorScene(float screenWidth, float screenHeight);
		~EditorScene() override;

		void OnUpdate(Engine::Timestep ts) override;

		// --- JS bridge entry points ------------------------------------------
		ObjectId LoadSplatFromBytes(const uint8_t* data, size_t size, const std::string& name);
		ObjectId LoadMeshFromBytes (const uint8_t* data, size_t size, const std::string& name);
		ObjectId AddLight(const std::string& kind);
		void     SetLightProps(ObjectId id, const glm::vec3& color, float intensity);
		void     DeleteObject(ObjectId id);
		void     SelectObject(ObjectId id);
		void     FocusObject(ObjectId id);
		void     RenameObject(ObjectId id, const std::string& name);
		void     SetVisibility(ObjectId id, bool visible);
		void     SetTransform(ObjectId id, const glm::vec3& pos,
		                      const glm::vec3& eulerDeg, const glm::vec3& scale);
		void     SetSnap(bool snap);
		// Teleport the fly camera to (pos, looking along fwd). React calls
		// this from the manifest hydrate path and from inspector edits.
		void     SetCameraPose(const glm::vec3& pos, const glm::vec3& fwd);
		void     ClearAll();

		static EditorScene* Current() { return s_Current; }

	private:
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
			glm::vec3        dragPivot{0.0f};
			float            startProjectionT = 0.0f;
			float            startAngleRad    = 0.0f;
			float            currentAngleRad  = 0.0f;
			float            startDistPx      = 0.0f;
		};

		void HandleHotkeys();
		void HandleMouseInteraction(const glm::vec2& fbViewport);
		void BuildSceneGizmos(const glm::vec2& viewport);

		std::string AutoNameFor(const std::string& filenameStem) const;
		EditorObject* FindObject(ObjectId id);
		const EditorObject* FindObject(ObjectId id) const;
		EditorObject* SelectedObject();

		// Object-space AABB for picking. Returns false for objects without
		// physical geometry (lights — pickable via a small sphere instead).
		bool GetLocalAabb(const EditorObject& o, glm::vec3& mn, glm::vec3& mx) const;
		void ComputeWorldAabb(const EditorObject& o, glm::vec3& outMin, glm::vec3& outMax) const;
		glm::vec3 GizmoPivot() const;

		// Returns the unit world-space direction a directional light shines
		// toward (i.e. -lightDir for shading). Reads the object's rotation.
		static glm::vec3 LightWorldDirection(const EditorObject& o);

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

		void PostObjectsList();
		void PostSelectionChanged();
		void PostTransformUpdate(bool isFinal);

		// Find the first visible directional light. Returns nullptr if none.
		const EditorObject* FirstActiveLight() const;

		// Manage a depth texture sized to the current viewport. Recreated
		// on resize. Used only by the mesh pass.
		void EnsureDepthTexture(uint32_t w, uint32_t h);

		std::vector<EditorObject>             m_Objects;
		ObjectId                              m_NextId    = 1;
		ObjectId                              m_Selected  = 0;
		bool                                  m_SnapToggle = false;
		GizmoHit                              m_Hover;
		DragState                             m_Drag;

		std::unique_ptr<Engine::GridRenderer>  m_Grid;
		std::unique_ptr<Engine::GizmoRenderer> m_Gizmo;

		// Depth attachment for the mesh pass. Sized to the swapchain.
		WGPUTexture     m_DepthTex     = nullptr;
		WGPUTextureView m_DepthView    = nullptr;
		uint32_t        m_DepthWidth   = 0;
		uint32_t        m_DepthHeight  = 0;

		bool      m_PrevLmb           = false;
		glm::vec2 m_LmbPressCursor{0.0f};

		static EditorScene* s_Current;
	};

}
