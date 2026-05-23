#include "EditorScene.h"
#include "SceneRegistry.h"

#include "Engine/Application.h"
#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/MouseButtonCodes.h"
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SplatLoader.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

		// Fly-camera default spawn — above the grid looking forward.
		const glm::vec3 kSpawnPos     = glm::vec3(0.0f, 2.5f, 6.0f);
		const glm::vec3 kSpawnForward = glm::normalize(glm::vec3(0.0f, -0.25f, -1.0f));

		// Pixel slop for "click vs drag" and gizmo hover.
		constexpr float kClickSlopPx     = 4.0f;
		constexpr float kAxisHitThreshPx = 10.0f;
		constexpr float kRingHitThreshPx = 8.0f;
		constexpr float kPointHitThreshPx = 12.0f;

		constexpr float kSnapTranslate = 0.25f;
		constexpr float kSnapRotateRad = glm::radians(15.0f);
		constexpr float kSnapScale     = 0.10f;

		// Gizmo geometric ratios. axis_length = world_scale.
		//   shaft 0..0.70 = translate arrow shaft
		//   ring radius  = 0.85
		//   cube center   = 1.00 (with cube side = 0.10)
		constexpr float kShaftFrac = 0.70f;
		constexpr float kRingFrac  = 0.85f;
		constexpr float kCubeFrac  = 1.00f;
		constexpr float kCubeSize  = 0.10f;
		constexpr float kCenterCubeSize = 0.08f;

		// Soft (Spline-style) axis colors. R/G/B but less saturated.
		const glm::vec4 kAxisCol[3] = {
			glm::vec4(0.88f, 0.38f, 0.42f, 0.95f),  // X — soft red
			glm::vec4(0.34f, 0.79f, 0.48f, 0.95f),  // Y — soft green
			glm::vec4(0.35f, 0.55f, 0.94f, 0.95f),  // Z — soft blue
		};
		const glm::vec4 kCenterCol = glm::vec4(0.78f, 0.78f, 0.82f, 0.90f);
		const glm::vec4 kBboxColSelected   = glm::vec4(0.85f, 0.85f, 0.90f, 0.85f);
		const glm::vec4 kBboxColUnselected = glm::vec4(0.55f, 0.55f, 0.60f, 0.25f);

		glm::vec4 HoverCol(const glm::vec4& c)
		{
			return glm::vec4(std::min(1.0f, c.r + 0.06f),
			                 std::min(1.0f, c.g + 0.06f),
			                 std::min(1.0f, c.b + 0.06f),
			                 1.0f);
		}

		bool LmbDown() { return Input::IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }
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

		// Cursor + viewport in CSS pixels relative to the canvas.
		// glfwGetCursorPos units vary between GLFW ports and DPRs, so we
		// read the cursor straight from the DOM via a one-time listener
		// and rescale via canvas.getBoundingClientRect.
		struct CursorReading {
			glm::vec2 cursor   = glm::vec2(0.0f);
			glm::vec2 viewport = glm::vec2(1.0f);
			bool      valid    = false;
		};

		CursorReading ReadCursorCss()
		{
			CursorReading r;
		#ifdef __EMSCRIPTEN__
			double cx = 0.0, cy = 0.0, vw = 0.0, vh = 0.0, ok = 0.0;
			EM_ASM({
				try {
					var cv = Module['canvas'] || document.querySelector('canvas.emscripten') || document.querySelector('canvas');
					if (!cv) return;
					var rect = cv.getBoundingClientRect();
					if (!window.__editorLastMouse) {
						var m0 = {};
						m0.x = 0;
						m0.y = 0;
						window.__editorLastMouse = m0;
						window.addEventListener('mousemove', function(ev) {
							window.__editorLastMouse.x = ev.clientX;
							window.__editorLastMouse.y = ev.clientY;
						});
					}
					var m = window.__editorLastMouse;
					setValue($0, m.x - rect.left, 'double');
					setValue($1, m.y - rect.top,  'double');
					setValue($2, rect.width,      'double');
					setValue($3, rect.height,     'double');
					setValue($4, 1.0,             'double');
				} catch (e) {}
			}, &cx, &cy, &vw, &vh, &ok);
			if (ok > 0.5 && vw > 0.0 && vh > 0.0) {
				r.cursor   = glm::vec2((float)cx, (float)cy);
				r.viewport = glm::vec2((float)vw, (float)vh);
				r.valid    = true;
			}
		#endif
			return r;
		}

		glm::vec3 CameraWorldPos(const SPtr<Camera>& cam)
		{
			return glm::vec3(glm::inverse(cam->GetViewMatrix())[3]);
		}

		void CursorRay(const SPtr<Camera>& cam, const glm::vec2& cursor,
		               const glm::vec2& viewport,
		               glm::vec3& outOrigin, glm::vec3& outDir)
		{
			const float ndcX = 2.0f * cursor.x / viewport.x - 1.0f;
			const float ndcY = 1.0f - 2.0f * cursor.y / viewport.y;
			const glm::mat4 inv = glm::inverse(cam->GetProjectionMatrix() * cam->GetViewMatrix());
			glm::vec4 nW = inv * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
			glm::vec4 fW = inv * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
			outOrigin = glm::vec3(nW) / nW.w;
			outDir    = glm::normalize(glm::vec3(fW) / fW.w - outOrigin);
		}

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

		float DistPointSegment(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b)
		{
			const glm::vec2 ab = b - a;
			const float denom = glm::dot(ab, ab);
			if (denom < 1e-6f) return glm::length(p - a);
			float t = glm::dot(p - a, ab) / denom;
			t = std::clamp(t, 0.0f, 1.0f);
			return glm::length(p - (a + ab * t));
		}

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

		bool RayPlane(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
		              const glm::vec3& p0, const glm::vec3& n,
		              glm::vec3& outHit, float& outT)
		{
			const float denom = glm::dot(rayDir, n);
			if (std::abs(denom) < 1e-6f) return false;
			const float t = glm::dot(p0 - rayOrigin, n) / denom;
			if (t < 0.0f) return false;
			outHit = rayOrigin + rayDir * t;
			outT = t;
			return true;
		}

		float SnapTo(float v, float step) { return std::round(v / step) * step; }

		void TransformAabb(const GaussianSplatRenderer::AABB& in,
		                   const glm::mat4& m,
		                   glm::vec3& outMin, glm::vec3& outMax)
		{
			const glm::vec3 corners[8] = {
				{ in.min.x, in.min.y, in.min.z }, { in.max.x, in.min.y, in.min.z },
				{ in.min.x, in.max.y, in.min.z }, { in.max.x, in.max.y, in.min.z },
				{ in.min.x, in.min.y, in.max.z }, { in.max.x, in.min.y, in.max.z },
				{ in.min.x, in.max.y, in.max.z }, { in.max.x, in.max.y, in.max.z },
			};
			outMin = glm::vec3( std::numeric_limits<float>::max());
			outMax = glm::vec3(-std::numeric_limits<float>::max());
			for (int i = 0; i < 8; ++i) {
				const glm::vec4 w = m * glm::vec4(corners[i], 1.0f);
				const glm::vec3 p(w);
				outMin = glm::min(outMin, p);
				outMax = glm::max(outMax, p);
			}
		}

		bool RayAabb(const glm::vec3& origin, const glm::vec3& dir,
		             const glm::vec3& mn, const glm::vec3& mx, float& outT)
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
			outT = tmin >= 0.0f ? tmin : tmax;
			return outT >= 0.0f;
		}

		// Filename helpers. Strip extension and any path prefix from a name
		// like "/tmp/cat.splat" → "cat".
		std::string StripFilename(const std::string& s)
		{
			auto slash = s.find_last_of("/\\");
			std::string base = (slash == std::string::npos) ? s : s.substr(slash + 1);
			auto dot = base.find_last_of('.');
			return (dot == std::string::npos) ? base : base.substr(0, dot);
		}

		// Build a TRS-aware AABB-corner-bracket primitive: short L-shapes
		// at every corner, pointing inward along each axis. `bracketLen`
		// is the world-space length of each L arm.
		void AddCornerBrackets(GizmoRenderer& giz,
		                       const glm::vec3& mn, const glm::vec3& mx,
		                       const glm::vec4& col, float thickness)
		{
			const glm::vec3 ext = mx - mn;
			const float bracketLen = std::min({ext.x, ext.y, ext.z}) * 0.08f;

			for (int cz = 0; cz < 2; ++cz) {
			for (int cy = 0; cy < 2; ++cy) {
			for (int cx = 0; cx < 2; ++cx) {
				const glm::vec3 corner(
					cx ? mx.x : mn.x,
					cy ? mx.y : mn.y,
					cz ? mx.z : mn.z);
				// Direction TOWARD the opposite corner along each axis.
				const glm::vec3 dirX = glm::vec3(cx ? -1.0f : 1.0f, 0.0f, 0.0f);
				const glm::vec3 dirY = glm::vec3(0.0f, cy ? -1.0f : 1.0f, 0.0f);
				const glm::vec3 dirZ = glm::vec3(0.0f, 0.0f, cz ? -1.0f : 1.0f);
				giz.AddLine(corner, corner + dirX * bracketLen, col, thickness);
				giz.AddLine(corner, corner + dirY * bracketLen, col, thickness);
				giz.AddLine(corner, corner + dirZ * bracketLen, col, thickness);
			}}}
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


	float EditorScene::GizmoWorldScale(const SPtr<Camera>& cam, const glm::vec3& pivot) const
	{
		const glm::vec3 camPos = CameraWorldPos(cam);
		const float dist = glm::length(camPos - pivot);
		return std::max(0.05f, dist * 0.18f);
	}


	EditorScene::EditorScene(float screenWidth, float screenHeight)
		: SceneBase("editor", screenWidth, screenHeight)
	{
		s_Current = this;
		m_CameraConfig.dragButton = MOUSE_BUTTON_RIGHT;
		SwitchCameraToFly(kSpawnPos, kSpawnForward);

		m_Grid  = std::make_unique<GridRenderer>(Application::Get().GetGfx());
		m_Gizmo = std::make_unique<GizmoRenderer>(Application::Get().GetGfx());

		INFO_CORE("EditorScene: ready (empty, fly-cam @ ({0:.2f},{1:.2f},{2:.2f}); RMB=camera)",
		          kSpawnPos.x, kSpawnPos.y, kSpawnPos.z);
		PostSceneMessage("{\"type\":\"splat-ready\"}");
		PostSceneMessage("{\"type\":\"editor-ready\"}");
		PostObjectsList();
	}


	EditorScene::~EditorScene()
	{
		if (s_Current == this) s_Current = nullptr;
	}


	// --- Object book-keeping ---------------------------------------------------

	EditorScene::EditorObject* EditorScene::FindObject(ObjectId id)
	{
		for (auto& o : m_Objects) if (o.id == id) return &o;
		return nullptr;
	}
	const EditorScene::EditorObject* EditorScene::FindObject(ObjectId id) const
	{
		for (const auto& o : m_Objects) if (o.id == id) return &o;
		return nullptr;
	}
	EditorScene::EditorObject* EditorScene::SelectedObject()
	{
		return m_Selected ? FindObject(m_Selected) : nullptr;
	}


	std::string EditorScene::AutoNameFor(const std::string& filenameStem) const
	{
		std::string base = filenameStem.empty() ? std::string("object") : filenameStem;
		// Walk indices until we find an unused name.
		auto exists = [&](const std::string& n) {
			for (const auto& o : m_Objects) if (o.name == n) return true;
			return false;
		};
		if (!exists(base)) return base;
		for (int i = 2; i < 9999; ++i) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%s (%d)", base.c_str(), i);
			std::string candidate(buf);
			if (!exists(candidate)) return candidate;
		}
		return base + " (?)";
	}


	void EditorScene::ComputeWorldAabb(const EditorObject& o,
	                                   glm::vec3& outMin, glm::vec3& outMax) const
	{
		if (!o.splat || !o.splat->BoundingBox().valid) {
			outMin = outMax = glm::vec3(0.0f);
			return;
		}
		TransformAabb(o.splat->BoundingBox(), o.transform.Matrix(), outMin, outMax);
	}


	glm::vec3 EditorScene::GizmoPivot() const
	{
		const auto* o = FindObject(m_Selected);
		if (!o || !o->splat) return glm::vec3(0.0f);
		glm::vec3 mn, mx;
		ComputeWorldAabb(*o, mn, mx);
		return (mn + mx) * 0.5f;
	}


	// --- Picking ---------------------------------------------------------------

	bool EditorScene::RaycastObject(const EditorObject& o, const glm::vec3& origin,
	                                const glm::vec3& dir, float& outT) const
	{
		if (!o.splat || !o.splat->BoundingBox().valid) return false;
		glm::vec3 mn, mx;
		ComputeWorldAabb(o, mn, mx);
		return RayAabb(origin, dir, mn, mx, outT);
	}


	EditorScene::ObjectId EditorScene::PickObject(const glm::vec3& origin,
	                                              const glm::vec3& dir) const
	{
		ObjectId best     = 0;
		float    bestT    = std::numeric_limits<float>::max();
		for (const auto& o : m_Objects) {
			if (!o.visible) continue;
			float t = 0.0f;
			if (RaycastObject(o, origin, dir, t) && t < bestT) {
				bestT = t;
				best  = o.id;
			}
		}
		return best;
	}


	EditorScene::GizmoHit EditorScene::PickGizmoHandle(const glm::vec2& cursor,
	                                                   const glm::vec2& viewport) const
	{
		GizmoHit best;
		if (!m_Selected) return best;

		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		const glm::vec3 pivot = GizmoPivot();
		const float L = GizmoWorldScale(cam, pivot);

		glm::vec2 pivotPx;
		if (!ProjectToScreen(pivot, cam, viewport, pivotPx)) return best;

		auto consider = [&](GizmoHit::Kind k, int axis, float dPx, float thresh) {
			if (dPx < thresh && dPx < best.distancePx) {
				best.kind = k;
				best.axis = axis;
				best.distancePx = dPx;
			}
		};

		// Translate arrow stems: pivot → pivot + axis * L * kShaftFrac.
		for (int a = 0; a < 3; ++a) {
			const glm::vec3 tip = pivot + AxisDir(a) * L * kShaftFrac;
			glm::vec2 tipPx;
			if (!ProjectToScreen(tip, cam, viewport, tipPx)) continue;
			const float d = DistPointSegment(cursor, pivotPx, tipPx);
			consider(GizmoHit::Kind::TranslateAxis, a, d, kAxisHitThreshPx);
		}

		// Rotate rings: project ~48 sample points and find closest chord.
		constexpr int kRingSamples = 48;
		for (int a = 0; a < 3; ++a) {
			const glm::vec3 axis = AxisDir(a);
			const glm::vec3 u =
				(std::abs(axis.x) < 0.5f) ? glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)))
				                          : glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
			const glm::vec3 v = glm::normalize(glm::cross(axis, u));
			glm::vec2 prev{};
			bool havePrev = false;
			for (int i = 0; i <= kRingSamples; ++i) {
				const float t = float(i) / float(kRingSamples);
				const float ang = t * glm::two_pi<float>();
				const glm::vec3 wp = pivot
					+ (std::cos(ang) * u + std::sin(ang) * v) * (L * kRingFrac);
				glm::vec2 px;
				if (!ProjectToScreen(wp, cam, viewport, px)) {
					havePrev = false;
					continue;
				}
				if (havePrev) {
					consider(GizmoHit::Kind::RotateRing, a,
					         DistPointSegment(cursor, prev, px), kRingHitThreshPx);
				}
				prev = px;
				havePrev = true;
			}
		}

		// Scale cubes: distance to projected cube center.
		for (int a = 0; a < 3; ++a) {
			const glm::vec3 c = pivot + AxisDir(a) * L * kCubeFrac;
			glm::vec2 px;
			if (!ProjectToScreen(c, cam, viewport, px)) continue;
			consider(GizmoHit::Kind::ScaleAxis, a, glm::length(cursor - px), kPointHitThreshPx);
		}

		// Center cube (uniform scale).
		consider(GizmoHit::Kind::ScaleUniform, 3, glm::length(cursor - pivotPx), kPointHitThreshPx);

		return best;
	}


	// --- Drag ------------------------------------------------------------------

	void EditorScene::BeginDrag(const GizmoHit& hit, const glm::vec2& cursor,
	                            const glm::vec2& viewport)
	{
		auto* o = SelectedObject();
		if (!o || hit.kind == GizmoHit::Kind::None) return;

		m_Drag.active         = true;
		m_Drag.kind           = hit.kind;
		m_Drag.axis           = hit.axis;
		m_Drag.objectId       = o->id;
		m_Drag.startCursor    = cursor;
		m_Drag.startTransform = o->transform;

		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		// Freeze pivot for the gesture — BeginDrag and UpdateDrag must agree.
		const glm::vec3 pivot = GizmoPivot();
		m_Drag.dragPivot = pivot;

		if (hit.kind == GizmoHit::Kind::TranslateAxis) {
			glm::vec3 ro, rd;
			CursorRay(cam, cursor, viewport, ro, rd);
			float t = 0.0f;
			ClosestAxisParam(ro, rd, pivot, AxisDir(hit.axis), t);
			m_Drag.startProjectionT = t;
		} else if (hit.kind == GizmoHit::Kind::RotateRing) {
			glm::vec3 ro, rd;
			CursorRay(cam, cursor, viewport, ro, rd);
			const glm::vec3 axis = AxisDir(hit.axis);
			glm::vec3 hp; float dt;
			if (RayPlane(ro, rd, pivot, axis, hp, dt)) {
				const glm::vec3 u = (std::abs(axis.x) < 0.5f)
					? glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)))
					: glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
				const glm::vec3 v = glm::normalize(glm::cross(axis, u));
				const glm::vec3 r = hp - pivot;
				m_Drag.startAngleRad = std::atan2(glm::dot(r, v), glm::dot(r, u));
				m_Drag.currentAngleRad = m_Drag.startAngleRad;
			}
		} else if (hit.kind == GizmoHit::Kind::ScaleAxis
		        || hit.kind == GizmoHit::Kind::ScaleUniform) {
			glm::vec2 pivotPx;
			ProjectToScreen(pivot, cam, viewport, pivotPx);
			m_Drag.startDistPx = std::max(1.0f, glm::length(cursor - pivotPx));
		}

		PostTransformUpdate(false);
	}


	void EditorScene::UpdateDrag(const glm::vec2& cursor, const glm::vec2& viewport)
	{
		auto* o = FindObject(m_Drag.objectId);
		if (!o) { m_Drag.active = false; return; }

		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		const glm::vec3 pivot0 = m_Drag.dragPivot;
		const bool snap = m_SnapToggle || CtrlDown();

		if (m_Drag.kind == GizmoHit::Kind::TranslateAxis) {
			glm::vec3 ro, rd;
			CursorRay(cam, cursor, viewport, ro, rd);
			const glm::vec3 axisDir = AxisDir(m_Drag.axis);
			float t = 0.0f;
			if (!ClosestAxisParam(ro, rd, pivot0, axisDir, t)) return;
			float delta = t - m_Drag.startProjectionT;
			if (snap) delta = SnapTo(delta, kSnapTranslate);
			o->transform.position = m_Drag.startTransform.position + axisDir * delta;
		} else if (m_Drag.kind == GizmoHit::Kind::RotateRing) {
			glm::vec3 ro, rd;
			CursorRay(cam, cursor, viewport, ro, rd);
			const glm::vec3 axis = AxisDir(m_Drag.axis);
			glm::vec3 hp; float dt;
			if (!RayPlane(ro, rd, pivot0, axis, hp, dt)) return;
			const glm::vec3 u = (std::abs(axis.x) < 0.5f)
				? glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)))
				: glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
			const glm::vec3 v = glm::normalize(glm::cross(axis, u));
			const glm::vec3 r = hp - pivot0;
			const float ang = std::atan2(glm::dot(r, v), glm::dot(r, u));
			float delta = ang - m_Drag.startAngleRad;
			while (delta >  glm::pi<float>()) delta -= glm::two_pi<float>();
			while (delta < -glm::pi<float>()) delta += glm::two_pi<float>();
			if (snap) delta = SnapTo(delta, kSnapRotateRad);
			m_Drag.currentAngleRad = m_Drag.startAngleRad + delta;
			const glm::quat dq = glm::angleAxis(delta, glm::normalize(axis));
			o->transform.rotation = glm::normalize(dq * m_Drag.startTransform.rotation);
			// Rotate-about-pivot: shift the object so the gizmo pivot is fixed.
			const glm::vec3 offset = m_Drag.startTransform.position - pivot0;
			o->transform.position = pivot0 + dq * offset;
		} else if (m_Drag.kind == GizmoHit::Kind::ScaleAxis
		        || m_Drag.kind == GizmoHit::Kind::ScaleUniform) {
			glm::vec2 pivotPx;
			if (!ProjectToScreen(pivot0, cam, viewport, pivotPx)) return;
			float currDist = std::max(1.0f, glm::length(cursor - pivotPx));
			float factor = currDist / m_Drag.startDistPx;
			if (snap) factor = std::max(0.01f, SnapTo(factor, kSnapScale));
			o->transform.scale = m_Drag.startTransform.scale;
			if (m_Drag.kind == GizmoHit::Kind::ScaleUniform || ShiftDown()) {
				o->transform.scale *= factor;
				// Uniform scale also pivots — keep gizmo center anchored.
				const glm::vec3 offset = m_Drag.startTransform.position - pivot0;
				o->transform.position = pivot0 + offset * factor;
			} else {
				o->transform.scale[m_Drag.axis] =
					m_Drag.startTransform.scale[m_Drag.axis] * factor;
			}
		}

		if (o->splat) o->splat->SetModelMatrix(o->transform.Matrix());
		PostTransformUpdate(false);
	}


	void EditorScene::EndDrag()
	{
		m_Drag.active = false;
		PostTransformUpdate(true);
	}


	// --- Per-frame -------------------------------------------------------------

	void EditorScene::HandleHotkeys()
	{
		// Delete key removes the selected object — handy when the table
		// row isn't focused (React panel sends a JS-bridge message in
		// that case; this is the in-canvas fallback).
		static bool prevDelete = false;
		const bool del = Input::IsKeyPressed(KEY_DELETE) || Input::IsKeyPressed(KEY_BACKSPACE);
		if (del && !prevDelete && m_Selected) {
			DeleteObject(m_Selected);
		}
		prevDelete = del;
	}


	void EditorScene::HandleMouseInteraction(const glm::vec2& fbViewport)
	{
		const auto reading = ReadCursorCss();
		glm::vec2 cursor   = reading.valid ? reading.cursor   : glm::vec2(0.0f);
		glm::vec2 viewport = reading.valid ? reading.viewport : fbViewport;
		if (!reading.valid) {
			const auto p = Input::GetMousePosition();
			cursor = glm::vec2(p.first, p.second);
		}
		const bool lmb = LmbDown();

		// Hover update while not dragging.
		if (!m_Drag.active) m_Hover = PickGizmoHandle(cursor, viewport);

		// LMB press.
		if (lmb && !m_PrevLmb) {
			m_LmbPressCursor = cursor;
			if (m_Hover.kind != GizmoHit::Kind::None) {
				BeginDrag(m_Hover, cursor, viewport);
			}
		}

		// LMB drag.
		if (lmb && m_Drag.active) UpdateDrag(cursor, viewport);

		// LMB release.
		if (!lmb && m_PrevLmb) {
			if (m_Drag.active) {
				EndDrag();
			} else if (glm::length(cursor - m_LmbPressCursor) < kClickSlopPx) {
				// Plain click — pick under cursor.
				glm::vec3 o, d;
				CursorRay(m_Camera->GetRenderCamera(), cursor, viewport, o, d);
				const ObjectId picked = PickObject(o, d);
				if (picked != m_Selected) {
					m_Selected = picked;
					PostSelectionChanged();
				}
			}
		}
		m_PrevLmb = lmb;
	}


	void EditorScene::BuildSceneGizmos(const glm::vec2& /*viewport*/)
	{
		m_Gizmo->Clear();
		if (m_Objects.empty()) return;

		// Bounding-box corner brackets per object.
		for (const auto& o : m_Objects) {
			if (!o.splat || !o.visible) continue;
			glm::vec3 mn, mx;
			ComputeWorldAabb(o, mn, mx);
			const bool sel = (o.id == m_Selected);
			AddCornerBrackets(*m_Gizmo, mn, mx,
			                  sel ? kBboxColSelected : kBboxColUnselected,
			                  sel ? 3.0f : 2.0f);
		}

		// Transform gizmo on the selected object.
		const auto* sel = FindObject(m_Selected);
		if (!sel || !sel->splat || !sel->visible) return;

		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		const glm::vec3 pivot = GizmoPivot();
		const float L = GizmoWorldScale(cam, pivot);

		auto isHover = [&](GizmoHit::Kind k, int a) {
			if (m_Drag.active) return m_Drag.kind == k && m_Drag.axis == a;
			return m_Hover.kind == k && m_Hover.axis == a;
		};

		const glm::vec3 camPos = CameraWorldPos(cam);

		for (int a = 0; a < 3; ++a) {
			const glm::vec4 baseCol = kAxisCol[a];
			const glm::vec3 axisDir = AxisDir(a);

			// Translate: chunky stem + big solid arrowhead.
			{
				const bool h = isHover(GizmoHit::Kind::TranslateAxis, a);
				const glm::vec4 c = h ? HoverCol(baseCol) : baseCol;
				const glm::vec3 tip = pivot + axisDir * L * kShaftFrac;
				m_Gizmo->AddLine(pivot, tip, c, 5.5f);
				m_Gizmo->AddArrowHead(tip, axisDir, L * 0.24f, L * 0.14f, camPos, c);
			}

			// Rotate: front-facing arc only.
			{
				const bool h = isHover(GizmoHit::Kind::RotateRing, a);
				const glm::vec4 c = h ? HoverCol(baseCol) : baseCol;
				const glm::vec3 u = (std::abs(axisDir.x) < 0.5f)
					? glm::normalize(glm::cross(axisDir, glm::vec3(1,0,0)))
					: glm::normalize(glm::cross(axisDir, glm::vec3(0,1,0)));
				const glm::vec3 v = glm::normalize(glm::cross(axisDir, u));
				const glm::vec3 toCam = glm::normalize(camPos - pivot);
				const float centerAng = std::atan2(glm::dot(toCam, v), glm::dot(toCam, u));
				const float halfSpan  = glm::radians(80.0f);
				m_Gizmo->AddArc(pivot, axisDir, L * kRingFrac,
				                centerAng - halfSpan, centerAng + halfSpan,
				                c, 32, 4.5f);
			}

			// Scale: filled disk at axis tip.
			{
				const bool h = isHover(GizmoHit::Kind::ScaleAxis, a);
				const glm::vec4 c = h ? HoverCol(baseCol) : baseCol;
				const glm::vec3 ballCenter = pivot + axisDir * L * kCubeFrac;
				m_Gizmo->AddDisk(ballCenter, L * 0.07f, camPos, c, 16);
			}
		}

		// Center disk for uniform scale.
		{
			const bool h = isHover(GizmoHit::Kind::ScaleUniform, 3);
			const glm::vec4 c = h ? HoverCol(kCenterCol) : kCenterCol;
			m_Gizmo->AddDisk(pivot, L * 0.055f, camPos, c, 16);
		}

		// Rotation arc while dragging — visualizes the swept angle.
		if (m_Drag.active && m_Drag.kind == GizmoHit::Kind::RotateRing) {
			const int a = m_Drag.axis;
			const glm::vec3 axis = AxisDir(a);
			const glm::vec3 u =
				(std::abs(axis.x) < 0.5f) ? glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)))
				                          : glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
			const glm::vec3 v = glm::normalize(glm::cross(axis, u));

			float a0 = m_Drag.startAngleRad;
			float a1 = m_Drag.currentAngleRad;
			float span = a1 - a0;
			int N = std::max(2, (int)std::ceil(std::abs(span) * 32.0f / glm::pi<float>()));
			const glm::vec4 fillCol = glm::vec4(kAxisCol[a].r, kAxisCol[a].g, kAxisCol[a].b, 0.5f);
			glm::vec3 prev = pivot + (std::cos(a0) * u + std::sin(a0) * v) * (L * kRingFrac);
			for (int i = 1; i <= N; ++i) {
				const float t = float(i) / float(N);
				const float ang = a0 + span * t;
				const glm::vec3 cur = pivot + (std::cos(ang) * u + std::sin(ang) * v) * (L * kRingFrac);
				// Filled-look: 3 lines per segment (radial wedge edges).
				m_Gizmo->AddLine(pivot, cur, fillCol, 1.5f);
				m_Gizmo->AddLine(prev, cur, glm::vec4(fillCol.r, fillCol.g, fillCol.b, 0.95f), 3.0f);
				prev = cur;
			}
		}
	}


	void EditorScene::OnUpdate(Timestep ts)
	{
		const glm::vec2 viewport = glm::vec2(m_ScreenWidth, m_ScreenHeight);

		HandleHotkeys();
		// Editor only has the fly camera now — no orbit, no mode toggle.
		HandleMouseInteraction(viewport);
		m_Camera->Update(ts);

		const SPtr<Camera> activeCam = m_Camera->GetRenderCamera();

		// Per-object frame-interval samples + perf tick for the FIRST splat
		// only (perf overlay is a single set of metrics; later we'll need to
		// scope this to selected, but for now the first object wins).
		static double s_PrevFrameStart = 0.0;
		const double frameStart = glfwGetTime();
		Engine::GaussianSplatRenderer* perfSplat = nullptr;
		for (auto& o : m_Objects) { if (o.splat && o.visible) { perfSplat = o.splat.get(); break; } }
		if (perfSplat && s_PrevFrameStart > 0.0) {
			perfSplat->Metrics().frameMs.Push(
				static_cast<float>((frameStart - s_PrevFrameStart) * 1000.0));
		}
		s_PrevFrameStart = frameStart;
		if (perfSplat) perfSplat->TickPerf();

		if (!Renderer::BeginScene(activeCam)) return;

		// Sort + draw EVERY visible object. Each owns its own renderer.
		const glm::mat4 view = activeCam->GetViewMatrix();
		const glm::mat4 proj = activeCam->GetProjectionMatrix();
		for (auto& o : m_Objects) {
			if (!o.splat || !o.visible) continue;
			o.splat->SetModelMatrix(o.transform.Matrix());
			o.splat->EncodeSort(Renderer::Encoder(), view, proj);
		}

		const WGPUPassTimestampWrites* renderTw =
			perfSplat ? perfSplat->GetRenderPassTimestampWrites() : nullptr;
		Renderer::OpenColorPass(0.12f, 0.13f, 0.16f, 1.0f, renderTw);

		if (m_Grid) m_Grid->EncodeRender(Renderer::CurrentPass(), activeCam);
		for (auto& o : m_Objects) {
			if (!o.splat || !o.visible) continue;
			o.splat->EncodeRender(Renderer::CurrentPass(), activeCam, viewport);
		}

		BuildSceneGizmos(viewport);
		if (m_Gizmo) m_Gizmo->EncodeRender(Renderer::CurrentPass(), activeCam, viewport);

		Renderer::ClosePass();
		if (perfSplat) perfSplat->ResolveAndReadTimestamps(Renderer::Encoder());

		if (perfSplat) {
			auto& m = perfSplat->Metrics();
			m.splatCount = static_cast<int>(perfSplat->SplatCount());
			const glm::vec3 eye = m_Camera->GetPosition();
			m.camEye[0] = eye.x; m.camEye[1] = eye.y; m.camEye[2] = eye.z;
			m.Emit();
		}

		Renderer::EndScene();
	}


	// --- Public API ------------------------------------------------------------

	EditorScene::ObjectId EditorScene::LoadSplatFromBytes(const uint8_t* data, size_t size,
	                                                      const std::string& sourceName)
	{
		INFO_CORE("EditorScene: parsing {0} byte splat payload (name='{1}')",
		          (uint64_t)size, sourceName);
		SplatData parsed = SplatLoader::LoadSplatFromBytes(data, size, "editor-upload");
		if (parsed.Empty()) {
			ERROR_CORE("EditorScene: splat parse returned no points");
			PostSceneMessage("{\"type\":\"editor-error\",\"message\":\"Failed to parse splat\"}");
			return 0;
		}

		EditorObject o;
		o.id    = m_NextId++;
		o.name  = AutoNameFor(StripFilename(sourceName));
		o.splat = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		o.splat->Upload(parsed);
		// Center XZ on the centroid; lift Y so the bbox bottom sits on the grid.
		const glm::vec3 c = o.splat->Centroid();
		o.transform.position.x = -c.x;
		o.transform.position.z = -c.z;
		const auto& bb = o.splat->BoundingBox();
		o.transform.position.y = bb.valid ? -bb.min.y : -c.y;
		o.splat->SetModelMatrix(o.transform.Matrix());
		o.visible = true;

		const ObjectId id = o.id;
		m_Objects.push_back(std::move(o));
		m_Selected = id;

		INFO_CORE("EditorScene: loaded object id={0} ('{1}'), total objects={2}",
		          (uint64_t)id, FindObject(id)->name, (uint64_t)m_Objects.size());

		PostObjectsList();
		PostSelectionChanged();
		PostTransformUpdate(true);
		return id;
	}


	void EditorScene::DeleteObject(ObjectId id)
	{
		if (!id) return;
		auto it = std::find_if(m_Objects.begin(), m_Objects.end(),
		                       [&](const EditorObject& o){ return o.id == id; });
		if (it == m_Objects.end()) return;
		const bool wasSelected = (m_Selected == id);
		m_Objects.erase(it);
		if (wasSelected) {
			m_Selected = m_Objects.empty() ? 0 : m_Objects.front().id;
		}
		PostObjectsList();
		PostSelectionChanged();
		PostTransformUpdate(true);
	}


	void EditorScene::SelectObject(ObjectId id)
	{
		// Allow id=0 to deselect.
		if (id != 0 && !FindObject(id)) return;
		if (m_Selected == id) return;
		m_Selected = id;
		PostSelectionChanged();
		PostTransformUpdate(true);
	}


	void EditorScene::FocusObject(ObjectId id)
	{
		const auto* o = FindObject(id);
		if (!o || !o->splat) return;
		glm::vec3 mn, mx;
		ComputeWorldAabb(*o, mn, mx);
		const glm::vec3 center = (mn + mx) * 0.5f;
		const float diag = std::max(0.5f, glm::length(mx - mn));
		// Position the camera back along its current forward by ~1.6 diagonals.
		const glm::vec3 fwd = m_Camera->GetForward();
		const glm::vec3 eye = center - fwd * (diag * 1.6f);
		SwitchCameraToFly(eye, glm::normalize(center - eye));
		// Re-select if needed so the gizmo shows.
		if (m_Selected != id) {
			m_Selected = id;
			PostSelectionChanged();
		}
	}


	void EditorScene::RenameObject(ObjectId id, const std::string& name)
	{
		auto* o = FindObject(id);
		if (!o) return;
		if (name.empty()) return;
		o->name = name;
		PostObjectsList();
	}


	void EditorScene::SetVisibility(ObjectId id, bool visible)
	{
		auto* o = FindObject(id);
		if (!o) return;
		o->visible = visible;
		PostObjectsList();
	}


	void EditorScene::SetTransform(ObjectId id, const glm::vec3& pos,
	                               const glm::vec3& eulerDeg, const glm::vec3& scale)
	{
		auto* o = FindObject(id);
		if (!o) return;
		o->transform.position = pos;
		o->transform.rotation = glm::quat(glm::radians(eulerDeg));
		o->transform.scale    = scale;
		if (o->splat) o->splat->SetModelMatrix(o->transform.Matrix());
		PostTransformUpdate(true);
	}


	void EditorScene::SetSnap(bool snap)
	{
		m_SnapToggle = snap;
		char buf[64];
		std::snprintf(buf, sizeof(buf), "{\"type\":\"editor-snap\",\"on\":%s}",
		              snap ? "true" : "false");
		PostSceneMessage(buf);
	}


	void EditorScene::ClearAll()
	{
		m_Objects.clear();
		m_Selected = 0;
		PostObjectsList();
		PostSelectionChanged();
	}


	// --- Posting ---------------------------------------------------------------

	void EditorScene::PostObjectsList()
	{
		// Compact JSON — keep within a reasonable stack buffer.
		std::string body = "{\"type\":\"editor-objects\",\"objects\":[";
		bool first = true;
		for (const auto& o : m_Objects) {
			char row[256];
			std::snprintf(row, sizeof(row),
			              "%s{\"id\":%llu,\"name\":\"%s\",\"visible\":%s,\"count\":%llu}",
			              first ? "" : ",",
			              (unsigned long long)o.id,
			              o.name.c_str(),
			              o.visible ? "true" : "false",
			              (unsigned long long)(o.splat ? o.splat->SplatCount() : 0));
			body += row;
			first = false;
		}
		body += "]}";
		PostSceneMessage(body.c_str());
	}


	void EditorScene::PostSelectionChanged()
	{
		char buf[128];
		std::snprintf(buf, sizeof(buf),
		              "{\"type\":\"editor-selection-changed\",\"id\":%llu}",
		              (unsigned long long)m_Selected);
		PostSceneMessage(buf);
	}


	void EditorScene::PostTransformUpdate(bool isFinal)
	{
		const auto* o = SelectedObject();
		const glm::vec3 pos = o ? o->transform.position : glm::vec3(0.0f);
		const glm::vec3 scl = o ? o->transform.scale    : glm::vec3(1.0f);
		const glm::vec3 eulerDeg = glm::degrees(
			o ? glm::eulerAngles(o->transform.rotation) : glm::vec3(0.0f));

		const char* kindStr =
			m_Drag.kind == GizmoHit::Kind::TranslateAxis ? "translate"
			: m_Drag.kind == GizmoHit::Kind::RotateRing   ? "rotate"
			: m_Drag.kind == GizmoHit::Kind::ScaleAxis    ? "scale"
			: m_Drag.kind == GizmoHit::Kind::ScaleUniform ? "scale"
			: "none";
		const int axis = m_Drag.active ? m_Drag.axis : -1;

		char buf[512];
		std::snprintf(buf, sizeof(buf),
		              "{\"type\":\"editor-transform\","
		              "\"final\":%s,\"drag\":%s,\"kind\":\"%s\",\"axis\":%d,"
		              "\"id\":%llu,"
		              "\"position\":[%.4f,%.4f,%.4f],"
		              "\"rotationDeg\":[%.3f,%.3f,%.3f],"
		              "\"scale\":[%.4f,%.4f,%.4f]}",
		              isFinal ? "true" : "false",
		              m_Drag.active ? "true" : "false",
		              kindStr, axis,
		              (unsigned long long)m_Selected,
		              pos.x, pos.y, pos.z,
		              eulerDeg.x, eulerDeg.y, eulerDeg.z,
		              scl.x, scl.y, scl.z);
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

EDITOR_EXPORT void editor_load_splat_bytes(uint8_t* data, int len, const char* name)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	if (!data || len <= 0) return;
	s->LoadSplatFromBytes(data, static_cast<size_t>(len), name ? name : "");
}

EDITOR_EXPORT void editor_clear_scene(void)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->ClearAll();
}

EDITOR_EXPORT void editor_select_object(double id)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->SelectObject(static_cast<::Sandbox::EditorScene::ObjectId>(id));
}

EDITOR_EXPORT void editor_delete_object(double id)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->DeleteObject(static_cast<::Sandbox::EditorScene::ObjectId>(id));
}

EDITOR_EXPORT void editor_focus_object(double id)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->FocusObject(static_cast<::Sandbox::EditorScene::ObjectId>(id));
}

EDITOR_EXPORT void editor_rename_object(double id, const char* name)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s || !name) return;
	s->RenameObject(static_cast<::Sandbox::EditorScene::ObjectId>(id), name);
}

EDITOR_EXPORT void editor_set_visibility(double id, int visible)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->SetVisibility(static_cast<::Sandbox::EditorScene::ObjectId>(id), visible != 0);
}

EDITOR_EXPORT void editor_set_transform(double id,
                                        double px, double py, double pz,
                                        double rx, double ry, double rz,
                                        double sx, double sy, double sz)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->SetTransform(static_cast<::Sandbox::EditorScene::ObjectId>(id),
	                glm::vec3((float)px, (float)py, (float)pz),
	                glm::vec3((float)rx, (float)ry, (float)rz),
	                glm::vec3((float)sx, (float)sy, (float)sz));
}

EDITOR_EXPORT void editor_set_snap(int onOff)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->SetSnap(onOff != 0);
}

} // extern "C"
