#include "EditorScene.h"
#include "SceneRegistry.h"

#include "Engine/Application.h"
#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/MouseButtonCodes.h"
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SplatLoader.h"
#include "Engine/VirtualInput.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace Engine;

namespace Sandbox {

	EditorScene* EditorScene::s_Current = nullptr;

	namespace {

		// Default fly-camera spawn — sits above the grid looking forward.
		const glm::vec3 kSpawnPos     = glm::vec3(0.0f, 2.5f, 6.0f);
		const glm::vec3 kSpawnForward = glm::normalize(glm::vec3(0.0f, -0.25f, -1.0f));

		// Pixel slop for "click vs drag" and gizmo hover.
		constexpr float kClickSlopPx     = 4.0f;
		constexpr float kAxisHitThreshPx = 9.0f;   // axes hover threshold
		constexpr float kRingHitThreshPx = 7.0f;   // rotate ring slop

		// Snap steps. The user spec'd 10% for scale; translate / rotate
		// use the conventional editor defaults.
		constexpr float kSnapTranslate = 0.25f;          // 25 cm
		constexpr float kSnapRotateRad = glm::radians(15.0f);
		constexpr float kSnapScale     = 0.10f;          // 10 %

		// Axis colors (RGBA). Same convention as every editor: X=red,
		// Y=green, Z=blue. Dimmed when not hovered.
		const glm::vec4 kAxisCol[3] = {
			glm::vec4(0.95f, 0.30f, 0.30f, 0.85f),
			glm::vec4(0.40f, 0.90f, 0.40f, 0.85f),
			glm::vec4(0.30f, 0.55f, 0.95f, 0.85f),
		};
		// Highlighted (hover / drag) — same hue, full alpha + boost.
		glm::vec4 HoverCol(int axis)
		{
			glm::vec4 c = kAxisCol[axis];
			c.a = 1.0f;
			c.r = std::min(1.0f, c.r + 0.05f);
			c.g = std::min(1.0f, c.g + 0.05f);
			c.b = std::min(1.0f, c.b + 0.05f);
			return c;
		}

		glm::vec2 GetCursor()
		{
			const auto p = Input::GetMousePosition();
			return glm::vec2(p.first, p.second);
		}

		bool LmbDown()      { return Input::IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }
		bool CtrlDown()
		{
			return Input::IsKeyPressed(KEY_LEFT_CONTROL)
			    || Input::IsKeyPressed(KEY_RIGHT_CONTROL);
		}
		bool ShiftDown()
		{
			return Input::IsKeyPressed(KEY_LEFT_SHIFT)
			    || Input::IsKeyPressed(KEY_RIGHT_SHIFT);
		}

		// World position of the camera (inverse of the view matrix' translation).
		glm::vec3 CameraWorldPos(const SPtr<Camera>& cam)
		{
			return glm::vec3(glm::inverse(cam->GetViewMatrix())[3]);
		}

		// Build a world-space ray from a cursor pixel (top-left origin)
		// through the camera. Returns (origin, normalized dir).
		void CursorRay(const SPtr<Camera>& cam, const glm::vec2& cursor,
		               const glm::vec2& viewport,
		               glm::vec3& outOrigin, glm::vec3& outDir)
		{
			const float ndcX = 2.0f * cursor.x / viewport.x - 1.0f;
			const float ndcY = 1.0f - 2.0f * cursor.y / viewport.y;

			const glm::mat4 inv = glm::inverse(cam->GetProjectionMatrix() * cam->GetViewMatrix());
			glm::vec4 nW = inv * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
			glm::vec4 fW = inv * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
			const glm::vec3 n3 = glm::vec3(nW) / nW.w;
			const glm::vec3 f3 = glm::vec3(fW) / fW.w;
			outOrigin = n3;
			outDir    = glm::normalize(f3 - n3);
		}

		// Project a world point to viewport pixels (top-left origin). Returns
		// false if the point is behind the camera or w == 0.
		bool ProjectToScreen(const glm::vec3& world,
		                     const SPtr<Camera>& cam,
		                     const glm::vec2& viewport,
		                     glm::vec2& outPx)
		{
			const glm::vec4 clip = cam->GetProjectionMatrix() * cam->GetViewMatrix()
			                       * glm::vec4(world, 1.0f);
			if (clip.w <= 1e-4f) return false;
			const float ndcX = clip.x / clip.w;
			const float ndcY = clip.y / clip.w;
			outPx.x = (ndcX * 0.5f + 0.5f) * viewport.x;
			outPx.y = (1.0f - (ndcY * 0.5f + 0.5f)) * viewport.y;
			return true;
		}

		// 2D pixel distance from `p` to segment (a,b).
		float DistPointSegment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b)
		{
			const glm::vec2 ab = b - a;
			const float denom = glm::dot(ab, ab);
			if (denom < 1e-6f) return glm::length(p - a);
			float t = glm::dot(p - a, ab) / denom;
			t = std::clamp(t, 0.0f, 1.0f);
			return glm::length(p - (a + ab * t));
		}

		// Closest point parameter t along axis line `o + d * t` to a ray.
		// Both d and rayDir should be unit vectors. Caller has to guard
		// against the parallel case (denom ≈ 0).
		bool ClosestAxisParam(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
		                      const glm::vec3& axisOrigin, const glm::vec3& axisDir,
		                      float& outT)
		{
			const glm::vec3 w0 = rayOrigin - axisOrigin;
			const float a = glm::dot(rayDir, rayDir);
			const float b = glm::dot(rayDir, axisDir);
			const float c = glm::dot(axisDir, axisDir);
			const float d = glm::dot(rayDir, w0);
			const float e = glm::dot(axisDir, w0);
			const float denom = a * c - b * b;
			if (std::abs(denom) < 1e-6f) return false;
			outT = (a * e - b * d) / denom;
			return true;
		}

		// Ray-plane intersect. Plane through `p0` with normal `n` (unit).
		// Returns true and writes hit position if the ray hits in front.
		bool RayPlane(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
		              const glm::vec3& p0, const glm::vec3& n,
		              glm::vec3& outHit)
		{
			const float denom = glm::dot(rayDir, n);
			if (std::abs(denom) < 1e-6f) return false;
			const float t = glm::dot(p0 - rayOrigin, n) / denom;
			if (t < 0.0f) return false;
			outHit = rayOrigin + rayDir * t;
			return true;
		}

		// Snap helpers — round to nearest step, returning the snapped value.
		float SnapTo(float v, float step) { return std::round(v / step) * step; }

		void TransformAABB(const GaussianSplatRenderer::AABB& in,
		                   const glm::mat4& m,
		                   glm::vec3& outMin, glm::vec3& outMax)
		{
			// Transform the 8 corners and refit. Loose but cheap.
			const glm::vec3 corners[8] = {
				{ in.min.x, in.min.y, in.min.z },
				{ in.max.x, in.min.y, in.min.z },
				{ in.min.x, in.max.y, in.min.z },
				{ in.max.x, in.max.y, in.min.z },
				{ in.min.x, in.min.y, in.max.z },
				{ in.max.x, in.min.y, in.max.z },
				{ in.min.x, in.max.y, in.max.z },
				{ in.max.x, in.max.y, in.max.z },
			};
			outMin = glm::vec3( std::numeric_limits<float>::max());
			outMax = glm::vec3(-std::numeric_limits<float>::max());
			for (int i = 0; i < 8; ++i) {
				const glm::vec4 w = m * glm::vec4(corners[i], 1.0f);
				const glm::vec3 p = glm::vec3(w);
				outMin = glm::min(outMin, p);
				outMax = glm::max(outMax, p);
			}
		}

		bool RayAabb(const glm::vec3& origin, const glm::vec3& dir,
		             const glm::vec3& mn, const glm::vec3& mx)
		{
			float tmin = -std::numeric_limits<float>::max();
			float tmax =  std::numeric_limits<float>::max();
			for (int i = 0; i < 3; ++i) {
				if (std::abs(dir[i]) < 1e-8f) {
					if (origin[i] < mn[i] || origin[i] > mx[i]) return false;
					continue;
				}
				const float inv = 1.0f / dir[i];
				float t1 = (mn[i] - origin[i]) * inv;
				float t2 = (mx[i] - origin[i]) * inv;
				if (t1 > t2) std::swap(t1, t2);
				tmin = std::max(tmin, t1);
				tmax = std::min(tmax, t2);
				if (tmin > tmax) return false;
			}
			return tmax >= 0.0f;
		}

	} // namespace


	glm::vec3 EditorScene::AxisDir(int axis)
	{
		switch (axis) {
			case 0: return glm::vec3(1, 0, 0);
			case 1: return glm::vec3(0, 1, 0);
			case 2: return glm::vec3(0, 0, 1);
			default: return glm::vec3(0, 1, 0);
		}
	}


	float EditorScene::GizmoWorldScale(const SPtr<Camera>& cam) const
	{
		// Distance from camera to gizmo pivot — gizmo grows / shrinks
		// linearly with distance so it stays the same pixel size on
		// screen regardless of camera zoom.
		const glm::vec3 camPos = CameraWorldPos(cam);
		const float dist = glm::length(camPos - GizmoPivot());
		// 0.18 ≈ takes ~25 % of viewport vertical at a 45° fovy.
		return std::max(0.05f, dist * 0.18f);
	}


	bool EditorScene::RaycastSplat(const glm::vec3& origin, const glm::vec3& dir) const
	{
		if (!m_Splats || !m_Splats->BoundingBox().valid) return false;
		glm::vec3 mn, mx;
		TransformAABB(m_Splats->BoundingBox(), m_Transform.Matrix(), mn, mx);
		return RayAabb(origin, dir, mn, mx);
	}


	EditorScene::GizmoHit EditorScene::PickGizmoHandle(const glm::vec2& cursor,
	                                                   const glm::vec2& viewport) const
	{
		GizmoHit best{};
		if (!m_Selected || !m_HasContent) return best;

		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		const glm::vec3 pivot = GizmoPivot();
		const float worldScale = GizmoWorldScale(cam);

		glm::vec2 pivotPx;
		const bool pivotVisible = ProjectToScreen(pivot, cam, viewport, pivotPx);

		auto considerAxis = [&](int axis, float distPx, float threshold) {
			if (distPx < threshold && distPx < best.distancePx) {
				best.axis = axis;
				best.distancePx = distPx;
			}
		};

		if (m_Tool == Tool::Translate || m_Tool == Tool::Scale) {
			// 3 axes — line segment hit test in pixel space.
			for (int axis = 0; axis < 3; ++axis) {
				const glm::vec3 tip = pivot + AxisDir(axis) * worldScale;
				glm::vec2 tipPx;
				if (!pivotVisible) continue;
				if (!ProjectToScreen(tip, cam, viewport, tipPx)) continue;
				const float d = DistPointSegment(cursor, pivotPx, tipPx);
				considerAxis(axis, d, kAxisHitThreshPx);
			}
		} else { // Rotate
			// For each axis, sample N points around the ring and find
			// minimum pixel distance.
			constexpr int kSamples = 48;
			for (int axis = 0; axis < 3; ++axis) {
				const glm::vec3 a = AxisDir(axis);
				// Build orthonormal basis in the ring plane.
				const glm::vec3 u =
					(std::abs(a.x) < 0.5f) ? glm::normalize(glm::cross(a, glm::vec3(1,0,0)))
					                       : glm::normalize(glm::cross(a, glm::vec3(0,1,0)));
				const glm::vec3 v = glm::normalize(glm::cross(a, u));
				glm::vec2 prev{};
				bool havePrev = false;
				for (int i = 0; i <= kSamples; ++i) {
					const float t = float(i) / float(kSamples);
					const float ang = t * glm::two_pi<float>();
					const glm::vec3 wp = pivot
						+ (std::cos(ang) * u + std::sin(ang) * v) * worldScale;
					glm::vec2 px;
					if (!ProjectToScreen(wp, cam, viewport, px)) {
						havePrev = false;
						continue;
					}
					if (havePrev) {
						const float d = DistPointSegment(cursor, prev, px);
						considerAxis(axis, d, kRingHitThreshPx);
					}
					prev = px;
					havePrev = true;
				}
			}
		}

		return best;
	}


	EditorScene::EditorScene(float screenWidth, float screenHeight)
		: SceneBase("editor", screenWidth, screenHeight)
	{
		s_Current = this;

		// Editor convention: RMB drives the camera so LMB stays free for
		// selection / gizmo. SceneBase reads this when spawning cameras.
		m_CameraConfig.dragButton = MOUSE_BUTTON_RIGHT;

		// Fly by default — orbit needs a subject and the scene starts empty.
		SwitchCameraToFly(kSpawnPos, kSpawnForward);

		m_Grid  = std::make_unique<GridRenderer>(Application::Get().GetGfx());
		m_Gizmo = std::make_unique<GizmoRenderer>(Application::Get().GetGfx());

		INFO_CORE("EditorScene: ready (empty, fly-cam @ ({0:.2f},{1:.2f},{2:.2f}); RMB=camera)",
		          kSpawnPos.x, kSpawnPos.y, kSpawnPos.z);

		PostSceneMessage("{\"type\":\"splat-ready\"}");
		PostSceneMessage("{\"type\":\"editor-ready\"}");
	}


	EditorScene::~EditorScene()
	{
		if (s_Current == this) s_Current = nullptr;
	}


	void EditorScene::HandleHotkeys()
	{
		// W/E/R switch tool (rising edge).
		const bool w = Input::IsKeyPressed(KEY_W);
		const bool e = Input::IsKeyPressed(KEY_E);
		const bool r = Input::IsKeyPressed(KEY_R);

		// Don't steal WASDEQ when the RMB camera is being used — pressing
		// W while flying shouldn't pop the user out of fly mode by
		// switching tools. We detect "camera using WASDEQ" by RMB held.
		const bool rmbHeld = Input::IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);

		if (!rmbHeld) {
			if (w && !m_PrevW) { m_Tool = Tool::Translate; PostToolChanged(); }
			if (e && !m_PrevE) { m_Tool = Tool::Rotate;    PostToolChanged(); }
			if (r && !m_PrevR) { m_Tool = Tool::Scale;     PostToolChanged(); }
		}
		m_PrevW = w; m_PrevE = e; m_PrevR = r;

		// Tab → snap camera back to orbit (custom: arbitration helper auto-
		// flips on WASDEQ, but for the editor we want Tab specifically).
		const bool tab = Input::IsKeyPressed(KEY_TAB);
		if (tab && !m_PrevTab && m_Camera->Mode() == Engine::CameraMode::Fly) {
			const Engine::PoseSnapshot s = m_Camera->Snapshot();
			SwitchCameraToOrbit(s.orbitTarget, s.position);
		}
		m_PrevTab = tab;
	}


	void EditorScene::HandleMouseInteraction(const glm::vec2& viewport)
	{
		const glm::vec2 cursor = GetCursor();
		const bool lmb = LmbDown();

		// Hover update (every frame while not dragging).
		if (!m_Drag.active) {
			m_Hover = PickGizmoHandle(cursor, viewport);
		}

		// LMB press edge.
		if (lmb && !m_PrevLmb) {
			m_LmbPressCursor       = cursor;
			m_LmbPressFromGizmo    = (m_Hover.axis >= 0);
			if (m_LmbPressFromGizmo) {
				BeginDrag(m_Hover.axis, cursor, viewport);
			}
		}

		// LMB drag (held + moved).
		if (lmb && m_Drag.active) {
			UpdateDrag(cursor, viewport);
		}

		// LMB release edge.
		if (!lmb && m_PrevLmb) {
			if (m_Drag.active) {
				EndDrag();
			} else if (glm::length(cursor - m_LmbPressCursor) < kClickSlopPx) {
				// Plain click — selection toggle.
				glm::vec3 o, d;
				CursorRay(m_Camera->GetRenderCamera(), cursor, viewport, o, d);
				const bool hit = RaycastSplat(o, d);
				const bool wasSelected = m_Selected;
				m_Selected = hit;
				if (m_Selected != wasSelected) PostSelectionChanged();
			}
		}

		m_PrevLmb = lmb;
	}


	void EditorScene::BeginDrag(int axis, const glm::vec2& cursor, const glm::vec2& viewport)
	{
		m_Drag.active         = true;
		m_Drag.tool           = m_Tool;
		m_Drag.axis           = axis;
		m_Drag.startCursor    = cursor;
		m_Drag.startTransform = m_Transform;

		const SPtr<Camera> cam = m_Camera->GetRenderCamera();

		if (m_Tool == Tool::Translate) {
			glm::vec3 o, d;
			CursorRay(cam, cursor, viewport, o, d);
			const glm::vec3 axisDir = AxisDir(axis);
			float t = 0.0f;
			ClosestAxisParam(o, d, GizmoPivot(), axisDir, t);
			m_Drag.startProjectionT = t;
		} else if (m_Tool == Tool::Rotate) {
			glm::vec3 o, d;
			CursorRay(cam, cursor, viewport, o, d);
			const glm::vec3 axisDir = AxisDir(axis);
			glm::vec3 hit;
			if (RayPlane(o, d, GizmoPivot(), axisDir, hit)) {
				const glm::vec3 u = (std::abs(axisDir.x) < 0.5f)
					? glm::normalize(glm::cross(axisDir, glm::vec3(1, 0, 0)))
					: glm::normalize(glm::cross(axisDir, glm::vec3(0, 1, 0)));
				const glm::vec3 v = glm::normalize(glm::cross(axisDir, u));
				const glm::vec3 r = hit - GizmoPivot();
				m_Drag.startAngleRad = std::atan2(glm::dot(r, v), glm::dot(r, u));
			} else {
				m_Drag.startAngleRad = 0.0f;
			}
		} else { // Scale
			glm::vec2 pivotPx;
			ProjectToScreen(GizmoPivot(), cam, viewport, pivotPx);
			m_Drag.startDistPx = std::max(1.0f, glm::length(cursor - pivotPx));
		}

		PostTransformUpdate(false);
	}


	void EditorScene::UpdateDrag(const glm::vec2& cursor, const glm::vec2& viewport)
	{
		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		const bool snap = m_SnapToggle || CtrlDown();

		if (m_Drag.tool == Tool::Translate) {
			glm::vec3 o, d;
			CursorRay(cam, cursor, viewport, o, d);
			const glm::vec3 axisDir = AxisDir(m_Drag.axis);
			float t = 0.0f;
			if (!ClosestAxisParam(o, d, m_Drag.startTransform.position, axisDir, t)) return;
			float delta = t - m_Drag.startProjectionT;
			if (snap) delta = SnapTo(delta, kSnapTranslate);
			m_Transform.position = m_Drag.startTransform.position + axisDir * delta;
		} else if (m_Drag.tool == Tool::Rotate) {
			glm::vec3 o, d;
			CursorRay(cam, cursor, viewport, o, d);
			const glm::vec3 axisDir = AxisDir(m_Drag.axis);
			glm::vec3 hit;
			if (!RayPlane(o, d, m_Drag.startTransform.position, axisDir, hit)) return;
			const glm::vec3 u = (std::abs(axisDir.x) < 0.5f)
				? glm::normalize(glm::cross(axisDir, glm::vec3(1, 0, 0)))
				: glm::normalize(glm::cross(axisDir, glm::vec3(0, 1, 0)));
			const glm::vec3 v = glm::normalize(glm::cross(axisDir, u));
			const glm::vec3 r = hit - m_Drag.startTransform.position;
			const float ang = std::atan2(glm::dot(r, v), glm::dot(r, u));
			float delta = ang - m_Drag.startAngleRad;
			if (snap) delta = SnapTo(delta, kSnapRotateRad);
			const glm::quat dq = glm::angleAxis(delta, glm::normalize(axisDir));
			m_Transform.rotation = glm::normalize(dq * m_Drag.startTransform.rotation);
		} else { // Scale
			glm::vec2 pivotPx;
			if (!ProjectToScreen(GizmoPivot(), cam, viewport, pivotPx)) return;
			float currDist = std::max(1.0f, glm::length(cursor - pivotPx));
			float factor   = currDist / m_Drag.startDistPx;
			if (snap) factor = std::max(0.01f, SnapTo(factor, kSnapScale));

			// Reset to start, then apply ratio to chosen axis (or all).
			m_Transform.scale = m_Drag.startTransform.scale;
			if (ShiftDown()) {
				// Uniform scale on Shift (Unity-style modifier).
				m_Transform.scale *= factor;
			} else {
				const int a = m_Drag.axis;
				m_Transform.scale[a] = m_Drag.startTransform.scale[a] * factor;
			}
		}

		// Apply transform to the splat renderer every drag tick — the
		// model matrix is what gives the user the live visual feedback.
		if (m_Splats) m_Splats->SetModelMatrix(m_Transform.Matrix());
		PostTransformUpdate(false);
	}


	void EditorScene::EndDrag()
	{
		m_Drag.active = false;
		m_Drag.axis   = -1;
		PostTransformUpdate(true);
	}


	void EditorScene::BuildGizmoPrimitives(const glm::vec2& /*viewport*/)
	{
		m_Gizmo->Clear();
		if (!m_Selected || !m_HasContent) return;

		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		const glm::vec3 pivot = GizmoPivot();
		const float L = GizmoWorldScale(cam);

		// Highlight currently hovered or dragged axis.
		const int activeAxis = m_Drag.active ? m_Drag.axis : m_Hover.axis;

		if (m_Tool == Tool::Translate) {
			for (int a = 0; a < 3; ++a) {
				const glm::vec4 c = (a == activeAxis) ? HoverCol(a) : kAxisCol[a];
				m_Gizmo->AddArrow(pivot, AxisDir(a), L, c, 4.0f);
			}
		} else if (m_Tool == Tool::Rotate) {
			for (int a = 0; a < 3; ++a) {
				const glm::vec4 c = (a == activeAxis) ? HoverCol(a) : kAxisCol[a];
				m_Gizmo->AddRing(pivot, AxisDir(a), L, c, 64, 3.0f);
			}
		} else { // Scale
			for (int a = 0; a < 3; ++a) {
				const glm::vec4 c = (a == activeAxis) ? HoverCol(a) : kAxisCol[a];
				const glm::vec3 tip = pivot + AxisDir(a) * L;
				m_Gizmo->AddLine(pivot, tip, c, 4.0f);
				m_Gizmo->AddWireCube(tip, L * 0.12f, c, 2.5f);
			}
			// Uniform-scale handle: small cube at pivot.
			const glm::vec4 cc = (activeAxis == 3) ? glm::vec4(1, 1, 1, 1)
			                                       : glm::vec4(0.85f, 0.85f, 0.85f, 0.85f);
			m_Gizmo->AddWireCube(pivot, L * 0.10f, cc, 2.5f);
		}
	}


	void EditorScene::OnUpdate(Timestep ts)
	{
		const glm::vec2 viewport = glm::vec2(m_ScreenWidth, m_ScreenHeight);

		HandleHotkeys();
		// Editor mode flips orbit→fly only via JS bridge toggle (no auto-
		// flip on WASDEQ — those keys belong to the camera while RMB is
		// held, but we don't want a stray W during click+drag to break
		// the mode).
		HandleStandardCameraArbitration(/*autoFlipOnFlyKey=*/false);
		DrainUnusedOrbitInput();
		HandleMouseInteraction(viewport);
		m_Camera->Update(ts);

		// Apply transform to splat renderer (cheap, sets matrix uniform).
		if (m_Splats) m_Splats->SetModelMatrix(m_Transform.Matrix());

		const SPtr<Camera> activeCam = m_Camera->GetRenderCamera();

		const double frameStart = glfwGetTime();
		if (m_Splats && m_PrevFrameStart > 0.0) {
			m_Splats->Metrics().frameMs.Push(
				static_cast<float>((frameStart - m_PrevFrameStart) * 1000.0));
		}
		m_PrevFrameStart = frameStart;
		if (m_Splats) m_Splats->TickPerf();

		if (!Renderer::BeginScene(activeCam)) return;

		const glm::mat4 view = activeCam->GetViewMatrix();
		const glm::mat4 proj = activeCam->GetProjectionMatrix();
		if (m_Splats) m_Splats->EncodeSort(Renderer::Encoder(), view, proj);

		const WGPUPassTimestampWrites* renderTw =
			m_Splats ? m_Splats->GetRenderPassTimestampWrites() : nullptr;
		Renderer::OpenColorPass(0.12f, 0.13f, 0.16f, 1.0f, renderTw);

		if (m_Grid) m_Grid->EncodeRender(Renderer::CurrentPass(), activeCam);
		if (m_Splats) {
			m_Splats->EncodeRender(Renderer::CurrentPass(),
			                       activeCam,
			                       viewport);
		}

		BuildGizmoPrimitives(viewport);
		if (m_Gizmo) m_Gizmo->EncodeRender(Renderer::CurrentPass(), activeCam, viewport);

		Renderer::ClosePass();

		if (m_Splats) m_Splats->ResolveAndReadTimestamps(Renderer::Encoder());

		if (m_Splats) {
			auto& m = m_Splats->Metrics();
			m.splatCount = static_cast<int>(m_SplatCount);
			const glm::vec3 eye = m_Camera->GetPosition();
			m.camEye[0] = eye.x; m.camEye[1] = eye.y; m.camEye[2] = eye.z;
			m.Emit();
		}

		++m_FpsCounter;
		double now = glfwGetTime();
		if (m_FpsT0 == 0.0) m_FpsT0 = now;
		if (m_FpsCounter % 120 == 0) {
			float dt = (float)(now - m_FpsT0);
			INFO_CORE("editor: 120 frames in {0:.3f}s = {1:.1f} fps", dt, 120.0f / dt);
			m_FpsT0 = now;
		}

		Renderer::EndScene();
	}


	void EditorScene::LoadSplatFromBytes(const uint8_t* data, size_t size)
	{
		INFO_CORE("EditorScene: parsing {0} byte splat payload", (uint64_t)size);
		SplatData parsed = SplatLoader::LoadSplatFromBytes(data, size, "editor-upload");
		if (parsed.Empty()) {
			ERROR_CORE("EditorScene: splat parse returned no points");
			PostSceneMessage("{\"type\":\"editor-error\",\"message\":\"Failed to parse splat\"}");
			return;
		}

		if (!m_Splats) {
			m_Splats = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		}
		m_Splats->Upload(parsed);
		m_SplatCount = parsed.Count();
		m_HasContent = true;

		// Reset transform — drop the splat's centroid at the WORLD origin so
		// the gizmo lands somewhere visible (the splat file's coordinate
		// frame often centers at the centroid already; if not, the user
		// can drag the gizmo wherever).
		m_Transform = Transform{};
		m_Transform.position = -m_Splats->Centroid();
		m_Splats->SetModelMatrix(m_Transform.Matrix());

		// Camera reframe — orbit around the centroid (which is now at
		// world origin after the transform).
		const glm::vec3 pivot = glm::vec3(0.0f);
		const float radius = 3.0f;
		const glm::vec3 eye = pivot + glm::vec3(0.0f, 0.4f, 1.0f) * radius;
		SwitchCameraToOrbit(pivot, eye);

		m_Selected = true;

		INFO_CORE("EditorScene: loaded {0} splats; world-aligned at origin", (uint64_t)m_SplatCount);
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		              "{\"type\":\"editor-splat-loaded\",\"count\":%llu}",
		              (unsigned long long)m_SplatCount);
		PostSceneMessage(buf);
		PostSelectionChanged();
		PostTransformUpdate(true);
	}


	void EditorScene::ClearScene()
	{
		m_Splats.reset();
		m_SplatCount = 0;
		m_HasContent = false;
		m_Selected   = false;
		m_Transform  = Transform{};
		PostSceneMessage("{\"type\":\"editor-scene-cleared\"}");
		PostSelectionChanged();
	}


	void EditorScene::SetTool(int tool)
	{
		if (tool < 0 || tool > 2) return;
		m_Tool = static_cast<Tool>(tool);
		PostToolChanged();
	}


	void EditorScene::SetSnap(bool snap)
	{
		m_SnapToggle = snap;
		char buf[64];
		std::snprintf(buf, sizeof(buf), "{\"type\":\"editor-snap\",\"on\":%s}",
		              snap ? "true" : "false");
		PostSceneMessage(buf);
	}


	void EditorScene::PostToolChanged()
	{
		const char* name = (m_Tool == Tool::Translate) ? "translate"
		                 : (m_Tool == Tool::Rotate)    ? "rotate" : "scale";
		char buf[80];
		std::snprintf(buf, sizeof(buf),
		              "{\"type\":\"editor-tool-changed\",\"tool\":\"%s\"}", name);
		PostSceneMessage(buf);
	}


	void EditorScene::PostSelectionChanged()
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		              "{\"type\":\"editor-selection-changed\",\"selected\":%s,\"hasContent\":%s,\"count\":%llu}",
		              m_Selected ? "true" : "false",
		              m_HasContent ? "true" : "false",
		              (unsigned long long)m_SplatCount);
		PostSceneMessage(buf);
	}


	void EditorScene::PostTransformUpdate(bool isFinal)
	{
		// Euler degrees for display (XYZ Tait-Bryan). The gizmo always
		// operates on the quaternion internally; euler is for the
		// inspector readout only.
		const glm::vec3 eulerRad = glm::eulerAngles(m_Transform.rotation);
		const glm::vec3 eulerDeg = glm::degrees(eulerRad);

		const char* tool = (m_Tool == Tool::Translate) ? "translate"
		                 : (m_Tool == Tool::Rotate)    ? "rotate" : "scale";
		const int  axis  = m_Drag.active ? m_Drag.axis : -1;

		char buf[512];
		std::snprintf(buf, sizeof(buf),
		              "{\"type\":\"editor-transform\","
		              "\"final\":%s,"
		              "\"drag\":%s,"
		              "\"tool\":\"%s\","
		              "\"axis\":%d,"
		              "\"position\":[%.4f,%.4f,%.4f],"
		              "\"rotationDeg\":[%.3f,%.3f,%.3f],"
		              "\"scale\":[%.4f,%.4f,%.4f]}",
		              isFinal ? "true" : "false",
		              m_Drag.active ? "true" : "false",
		              tool, axis,
		              m_Transform.position.x, m_Transform.position.y, m_Transform.position.z,
		              eulerDeg.x, eulerDeg.y, eulerDeg.z,
		              m_Transform.scale.x, m_Transform.scale.y, m_Transform.scale.z);
		PostSceneMessage(buf);
	}


	SCENE_REGISTER("editor", EditorScene)

} // namespace Sandbox


// ---------------------------------------------------------------------------
// C bridge — invoked from the JS shim in Sandbox.html via Module.ccall.
// ---------------------------------------------------------------------------

extern "C" {

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EDITOR_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EDITOR_EXPORT
#endif

EDITOR_EXPORT void editor_load_splat_bytes(uint8_t* data, int len)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) { ERROR_CORE("editor_load_splat_bytes: no live EditorScene"); return; }
	if (!data || len <= 0) {
		ERROR_CORE("editor_load_splat_bytes: bad args"); return;
	}
	s->LoadSplatFromBytes(data, static_cast<size_t>(len));
}

EDITOR_EXPORT void editor_clear_scene(void)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->ClearScene();
}

EDITOR_EXPORT void editor_set_tool(int tool)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->SetTool(tool);
}

EDITOR_EXPORT void editor_set_snap(int onOff)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->SetSnap(onOff != 0);
}

} // extern "C"
