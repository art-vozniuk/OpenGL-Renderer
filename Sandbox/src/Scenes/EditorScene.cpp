#include "EditorScene.h"
#include "SceneRegistry.h"

#include "Engine/Application.h"
#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/MouseButtonCodes.h"
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SplatLoader.h"
#include "Engine/Renderer/GltfLoader.h"

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

		const glm::vec3 kSpawnPos     = glm::vec3(0.0f, 2.5f, 6.0f);
		const glm::vec3 kSpawnForward = glm::normalize(glm::vec3(0.0f, -0.25f, -1.0f));

		constexpr float kClickSlopPx     = 4.0f;
		constexpr float kAxisHitThreshPx = 10.0f;
		constexpr float kRingHitThreshPx = 8.0f;
		constexpr float kPointHitThreshPx = 12.0f;

		constexpr float kSnapTranslate = 0.25f;
		constexpr float kSnapRotateRad = glm::radians(15.0f);
		constexpr float kSnapScale     = 0.10f;

		// Spline-style compact widget: arrow tip == scale sphere, rotate arcs
		// span between adjacent arm tips. All three handles share one arm length.
		constexpr float kArmFrac      = 0.85f;
		constexpr float kArrowHeadFrac = 0.22f;
		constexpr float kScaleSphereR  = 0.07f;

		// Light gizmo dimensions in world units.
		constexpr float kLightSphereR = 0.18f;
		constexpr float kLightRayLen  = 0.6f;
		// Light pick-bbox half-extent (world space).
		constexpr float kLightPickHalf = 0.25f;

		const glm::vec4 kAxisCol[3] = {
			glm::vec4(0.88f, 0.38f, 0.42f, 0.95f),
			glm::vec4(0.34f, 0.79f, 0.48f, 0.95f),
			glm::vec4(0.35f, 0.55f, 0.94f, 0.95f),
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
		bool CtrlDown() { return Input::IsKeyPressed(KEY_LEFT_CONTROL)  || Input::IsKeyPressed(KEY_RIGHT_CONTROL); }
		bool ShiftDown(){ return Input::IsKeyPressed(KEY_LEFT_SHIFT)    || Input::IsKeyPressed(KEY_RIGHT_SHIFT);   }

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

		void TransformAabb(const glm::vec3& mnIn, const glm::vec3& mxIn,
		                   const glm::mat4& m,
		                   glm::vec3& outMin, glm::vec3& outMax)
		{
			const glm::vec3 corners[8] = {
				{ mnIn.x, mnIn.y, mnIn.z }, { mxIn.x, mnIn.y, mnIn.z },
				{ mnIn.x, mxIn.y, mnIn.z }, { mxIn.x, mxIn.y, mnIn.z },
				{ mnIn.x, mnIn.y, mxIn.z }, { mxIn.x, mnIn.y, mxIn.z },
				{ mnIn.x, mxIn.y, mxIn.z }, { mxIn.x, mxIn.y, mxIn.z },
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

		std::string StripFilename(const std::string& s)
		{
			auto slash = s.find_last_of("/\\");
			std::string base = (slash == std::string::npos) ? s : s.substr(slash + 1);
			auto dot = base.find_last_of('.');
			return (dot == std::string::npos) ? base : base.substr(0, dot);
		}

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
				const glm::vec3 dirX = glm::vec3(cx ? -1.0f : 1.0f, 0.0f, 0.0f);
				const glm::vec3 dirY = glm::vec3(0.0f, cy ? -1.0f : 1.0f, 0.0f);
				const glm::vec3 dirZ = glm::vec3(0.0f, 0.0f, cz ? -1.0f : 1.0f);
				giz.AddLine(corner, corner + dirX * bracketLen, col, thickness);
				giz.AddLine(corner, corner + dirY * bracketLen, col, thickness);
				giz.AddLine(corner, corner + dirZ * bracketLen, col, thickness);
			}}}
		}

		inline WGPUStringView SV(const char* s)
		{
			WGPUStringView v{};
			v.data   = s;
			v.length = WGPU_STRLEN;
			return v;
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


	glm::vec3 EditorScene::LightWorldDirection(const EditorObject& o)
	{
		// Default direction: down + slight forward bias. Rotated by the
		// object's quaternion so users can orient the light via the gizmo.
		const glm::vec3 def(0.0f, -1.0f, 0.3f);
		return glm::normalize(o.transform.rotation * def);
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
		if (m_DepthView) { wgpuTextureViewRelease(m_DepthView); m_DepthView = nullptr; }
		if (m_DepthTex)  { wgpuTextureRelease(m_DepthTex);      m_DepthTex  = nullptr; }
		if (s_Current == this) s_Current = nullptr;
	}


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


	// Final render matrix: T * R * S * T(-pivotObj). Object-space points get
	// re-centered on the pivot first so transform.position is literally where
	// the pivot lands in world space.
	static glm::mat4 BuildObjectModelMatrix(const EditorScene::EditorObject& o)
	{
		return o.transform.Matrix()
		       * glm::translate(glm::mat4(1.0f), -o.pivotObj);
	}


	bool EditorScene::GetLocalAabb(const EditorObject& o, glm::vec3& mn, glm::vec3& mx) const
	{
		if (o.kind == Kind::Splat && o.splat && o.splat->BoundingBox().valid) {
			mn = o.splat->BoundingBox().min;
			mx = o.splat->BoundingBox().max;
			return true;
		}
		if (o.kind == Kind::Mesh && o.mesh && o.mesh->BoundingBox().valid) {
			mn = o.mesh->BoundingBox().min;
			mx = o.mesh->BoundingBox().max;
			return true;
		}
		if (o.kind == Kind::Light) {
			mn = glm::vec3(-kLightPickHalf);
			mx = glm::vec3( kLightPickHalf);
			return true;
		}
		return false;
	}


	void EditorScene::ComputeWorldAabb(const EditorObject& o,
	                                   glm::vec3& outMin, glm::vec3& outMax) const
	{
		glm::vec3 mn, mx;
		if (!GetLocalAabb(o, mn, mx)) {
			outMin = outMax = glm::vec3(0.0f);
			return;
		}
		TransformAabb(mn, mx, BuildObjectModelMatrix(o), outMin, outMax);
	}


	glm::vec3 EditorScene::GizmoPivot() const
	{
		const auto* o = FindObject(m_Selected);
		if (!o) return glm::vec3(0.0f);
		return o->transform.position;
	}


	bool EditorScene::RaycastObject(const EditorObject& o, const glm::vec3& origin,
	                                const glm::vec3& dir, float& outT) const
	{
		glm::vec3 mn, mx;
		ComputeWorldAabb(o, mn, mx);
		// For "empty" objects (kind without geometry), mn == mx; reject.
		if (mn == mx) return false;
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

		for (int a = 0; a < 3; ++a) {
			const glm::vec3 tip = pivot + AxisDir(a) * L * kArmFrac;
			glm::vec2 tipPx;
			if (!ProjectToScreen(tip, cam, viewport, tipPx)) continue;
			const float d = DistPointSegment(cursor, pivotPx, tipPx);
			consider(GizmoHit::Kind::TranslateAxis, a, d, kAxisHitThreshPx);
		}

		// Rotate arc spans between adjacent arm tips (90 deg quarter-circle).
		constexpr int kArcSamples = 24;
		for (int a = 0; a < 3; ++a) {
			const glm::vec3 uA = AxisDir((a + 1) % 3);
			const glm::vec3 vA = AxisDir((a + 2) % 3);
			glm::vec2 prev{};
			bool havePrev = false;
			for (int i = 0; i <= kArcSamples; ++i) {
				const float t = float(i) / float(kArcSamples);
				const float ang = t * glm::half_pi<float>();
				const glm::vec3 wp = pivot
					+ (std::cos(ang) * uA + std::sin(ang) * vA) * (L * kArmFrac);
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

		for (int a = 0; a < 3; ++a) {
			const glm::vec3 c = pivot + AxisDir(a) * L * kArmFrac;
			glm::vec2 px;
			if (!ProjectToScreen(c, cam, viewport, px)) continue;
			consider(GizmoHit::Kind::ScaleAxis, a, glm::length(cursor - px), kPointHitThreshPx);
		}

		consider(GizmoHit::Kind::ScaleUniform, 3, glm::length(cursor - pivotPx), kPointHitThreshPx);

		return best;
	}


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
				const glm::vec3 u = AxisDir((hit.axis + 1) % 3);
				const glm::vec3 v = AxisDir((hit.axis + 2) % 3);
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
			const glm::vec3 u = AxisDir((m_Drag.axis + 1) % 3);
			const glm::vec3 v = AxisDir((m_Drag.axis + 2) % 3);
			const glm::vec3 r = hp - pivot0;
			const float ang = std::atan2(glm::dot(r, v), glm::dot(r, u));
			float delta = ang - m_Drag.startAngleRad;
			while (delta >  glm::pi<float>()) delta -= glm::two_pi<float>();
			while (delta < -glm::pi<float>()) delta += glm::two_pi<float>();
			if (snap) delta = SnapTo(delta, kSnapRotateRad);
			m_Drag.currentAngleRad = m_Drag.startAngleRad + delta;
			const glm::quat dq = glm::angleAxis(delta, glm::normalize(axis));
			o->transform.rotation = glm::normalize(dq * m_Drag.startTransform.rotation);
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
			} else {
				o->transform.scale[m_Drag.axis] =
					m_Drag.startTransform.scale[m_Drag.axis] * factor;
			}
		}

		if (o->splat) o->splat->SetModelMatrix(BuildObjectModelMatrix(*o));
		if (o->mesh)  o->mesh->SetModelMatrix(BuildObjectModelMatrix(*o));
		PostTransformUpdate(false);
	}


	void EditorScene::EndDrag()
	{
		m_Drag.active = false;
		PostTransformUpdate(true);
	}


	void EditorScene::HandleHotkeys()
	{
		static bool prevDelete = false;
		const bool del = Input::IsKeyPressed(KEY_DELETE);
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

		if (!m_Drag.active) m_Hover = PickGizmoHandle(cursor, viewport);

		if (lmb && !m_PrevLmb) {
			m_LmbPressCursor = cursor;
			if (m_Hover.kind != GizmoHit::Kind::None) {
				BeginDrag(m_Hover, cursor, viewport);
			}
		}

		if (lmb && m_Drag.active) UpdateDrag(cursor, viewport);

		if (!lmb && m_PrevLmb) {
			if (m_Drag.active) {
				EndDrag();
			} else if (glm::length(cursor - m_LmbPressCursor) < kClickSlopPx) {
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

		// Bounding-box brackets per object (for everything with geometry).
		for (const auto& o : m_Objects) {
			if (!o.visible) continue;
			if (o.kind == Kind::Light) continue;
			glm::vec3 mn, mx;
			ComputeWorldAabb(o, mn, mx);
			if (mn == mx) continue;
			const bool sel = (o.id == m_Selected);
			AddCornerBrackets(*m_Gizmo, mn, mx,
			                  sel ? kBboxColSelected : kBboxColUnselected,
			                  sel ? 3.0f : 2.0f);
		}

		// Light gizmos: ring + rod indicating direction.
		const SPtr<Camera> cam = m_Camera->GetRenderCamera();
		const glm::vec3 camPos = CameraWorldPos(cam);
		for (const auto& o : m_Objects) {
			if (!o.visible || o.kind != Kind::Light) continue;
			const bool sel = (o.id == m_Selected);
			glm::vec3 baseColor = o.light ? o.light->color : glm::vec3(1.0f);
			glm::vec4 col(baseColor, sel ? 1.0f : 0.65f);

			const glm::vec3 c = o.transform.position;
			// Small disk facing camera.
			m_Gizmo->AddDisk(c, kLightSphereR * 0.6f, camPos, col, 16);
			// Ring around the light center.
			const glm::vec3 toCam = glm::normalize(camPos - c);
			m_Gizmo->AddRing(c, toCam, kLightSphereR, col, 24, sel ? 2.5f : 1.5f);
			// Direction rod + arrowhead.
			const glm::vec3 d = LightWorldDirection(o);
			const glm::vec3 tip = c + d * kLightRayLen;
			m_Gizmo->AddLine(c, tip, col, sel ? 3.0f : 2.0f);
			m_Gizmo->AddArrowHead(tip, d, 0.18f, 0.10f, camPos, col);
		}

		const auto* sel = FindObject(m_Selected);
		if (!sel || !sel->visible) return;

		const glm::vec3 pivot = GizmoPivot();
		const float L = GizmoWorldScale(cam, pivot);

		auto isHover = [&](GizmoHit::Kind k, int a) {
			if (m_Drag.active) return m_Drag.kind == k && m_Drag.axis == a;
			return m_Hover.kind == k && m_Hover.axis == a;
		};

		// Spline-style compact widget: three arms, each ending in a sphere at
		// the arrow tip; rotate arcs span between adjacent arm tips.
		const float arm     = L * kArmFrac;
		const float headLen = arm * kArrowHeadFrac;
		const float headW   = headLen * 0.55f;

		for (int a = 0; a < 3; ++a) {
			const glm::vec4 baseCol = kAxisCol[a];
			const glm::vec3 axisDir = AxisDir(a);
			const glm::vec3 tip     = pivot + axisDir * arm;
			const glm::vec3 shaftEnd = pivot + axisDir * (arm - headLen * 0.6f);

			{
				const bool h = isHover(GizmoHit::Kind::TranslateAxis, a);
				const glm::vec4 c = h ? HoverCol(baseCol) : baseCol;
				m_Gizmo->AddLine(pivot, shaftEnd, c, 5.5f);
				m_Gizmo->AddArrowHead(tip, axisDir, headLen, headW, camPos, c);
			}
			{
				const bool h = isHover(GizmoHit::Kind::ScaleAxis, a);
				const glm::vec4 c = h ? HoverCol(baseCol) : baseCol;
				m_Gizmo->AddDisk(tip, L * kScaleSphereR, camPos, c, 16);
			}
		}

		// Rotate arcs: 90 deg quarter-circle from arm-tip(a+1) to arm-tip(a+2),
		// in the plane perpendicular to axis a.
		for (int a = 0; a < 3; ++a) {
			const bool h = isHover(GizmoHit::Kind::RotateRing, a);
			const glm::vec4 c = h ? HoverCol(kAxisCol[a]) : kAxisCol[a];
			const glm::vec3 uA = AxisDir((a + 1) % 3);
			const glm::vec3 vA = AxisDir((a + 2) % 3);
			constexpr int kArcSeg = 16;
			glm::vec3 prev = pivot + uA * arm;
			for (int i = 1; i <= kArcSeg; ++i) {
				const float t = float(i) / float(kArcSeg);
				const float ang = t * glm::half_pi<float>();
				const glm::vec3 cur = pivot + (std::cos(ang) * uA + std::sin(ang) * vA) * arm;
				m_Gizmo->AddLine(prev, cur, c, 4.5f);
				prev = cur;
			}
		}

		{
			const bool h = isHover(GizmoHit::Kind::ScaleUniform, 3);
			const glm::vec4 c = h ? HoverCol(kCenterCol) : kCenterCol;
			m_Gizmo->AddDisk(pivot, L * 0.055f, camPos, c, 16);
		}

		if (m_Drag.active && m_Drag.kind == GizmoHit::Kind::RotateRing) {
			const int a = m_Drag.axis;
			const glm::vec3 u = AxisDir((a + 1) % 3);
			const glm::vec3 v = AxisDir((a + 2) % 3);

			float a0 = m_Drag.startAngleRad;
			float a1 = m_Drag.currentAngleRad;
			float span = a1 - a0;
			int N = std::max(2, (int)std::ceil(std::abs(span) * 32.0f / glm::pi<float>()));
			const glm::vec4 fillCol = glm::vec4(kAxisCol[a].r, kAxisCol[a].g, kAxisCol[a].b, 0.5f);
			glm::vec3 prev = pivot + (std::cos(a0) * u + std::sin(a0) * v) * arm;
			for (int i = 1; i <= N; ++i) {
				const float t = float(i) / float(N);
				const float ang = a0 + span * t;
				const glm::vec3 cur = pivot + (std::cos(ang) * u + std::sin(ang) * v) * arm;
				m_Gizmo->AddLine(pivot, cur, fillCol, 1.5f);
				m_Gizmo->AddLine(prev, cur, glm::vec4(fillCol.r, fillCol.g, fillCol.b, 0.95f), 3.0f);
				prev = cur;
			}
		}
	}


	const EditorScene::EditorObject* EditorScene::FirstActiveLight() const
	{
		for (const auto& o : m_Objects) {
			if (o.visible && o.kind == Kind::Light) return &o;
		}
		return nullptr;
	}


	void EditorScene::EnsureDepthTexture(uint32_t w, uint32_t h)
	{
		if (w == 0 || h == 0) return;
		if (m_DepthTex && m_DepthWidth == w && m_DepthHeight == h) return;
		if (m_DepthView) { wgpuTextureViewRelease(m_DepthView); m_DepthView = nullptr; }
		if (m_DepthTex)  { wgpuTextureRelease(m_DepthTex);      m_DepthTex  = nullptr; }

		WGPUContext& ctx = Application::Get().GetGfx();
		WGPUTextureDescriptor td{};
		td.label = SV("editor-depth");
		td.usage = WGPUTextureUsage_RenderAttachment;
		td.dimension = WGPUTextureDimension_2D;
		td.size.width  = w;
		td.size.height = h;
		td.size.depthOrArrayLayers = 1;
		td.format = MeshRenderer::DepthFormat();
		td.mipLevelCount = 1;
		td.sampleCount   = 1;
		m_DepthTex = wgpuDeviceCreateTexture(ctx.Device(), &td);

		WGPUTextureViewDescriptor vd{};
		vd.format = td.format;
		vd.dimension = WGPUTextureViewDimension_2D;
		vd.aspect = WGPUTextureAspect_DepthOnly;
		vd.mipLevelCount = 1;
		vd.arrayLayerCount = 1;
		m_DepthView = wgpuTextureCreateView(m_DepthTex, &vd);

		m_DepthWidth = w;
		m_DepthHeight = h;
	}


	void EditorScene::OnUpdate(Timestep ts)
	{
		const glm::vec2 viewport = glm::vec2(m_ScreenWidth, m_ScreenHeight);

		HandleHotkeys();
		HandleMouseInteraction(viewport);
		m_Camera->Update(ts);

		const SPtr<Camera> activeCam = m_Camera->GetRenderCamera();

		// First-splat perf instrumentation (same as before, just generalised to find any splat).
		static double s_PrevFrameStart = 0.0;
		const double frameStart = glfwGetTime();
		Engine::GaussianSplatRenderer* perfSplat = nullptr;
		for (auto& o : m_Objects) {
			if (o.kind == Kind::Splat && o.splat && o.visible) { perfSplat = o.splat.get(); break; }
		}
		if (perfSplat && s_PrevFrameStart > 0.0) {
			perfSplat->Metrics().frameMs.Push(
				static_cast<float>((frameStart - s_PrevFrameStart) * 1000.0));
		}
		s_PrevFrameStart = frameStart;
		if (perfSplat) perfSplat->TickPerf();

		if (!Renderer::BeginScene(activeCam)) return;

		const glm::mat4 view = activeCam->GetViewMatrix();
		const glm::mat4 proj = activeCam->GetProjectionMatrix();

		// 1) Splat sort encoding (compute) — must happen before any render
		//    pass is open.
		for (auto& o : m_Objects) {
			if (o.kind != Kind::Splat || !o.splat || !o.visible) continue;
			o.splat->SetModelMatrix(BuildObjectModelMatrix(o));
			o.splat->EncodeSort(Renderer::Encoder(), view, proj);
		}

		// 2) Splat / grid pass (no depth — existing pipelines). Gizmo deferred
		//    to a final overlay pass so it draws on top of meshes.
		const WGPUPassTimestampWrites* renderTw =
			perfSplat ? perfSplat->GetRenderPassTimestampWrites() : nullptr;
		Renderer::OpenColorPass(0.12f, 0.13f, 0.16f, 1.0f, renderTw);

		if (m_Grid) m_Grid->EncodeRender(Renderer::CurrentPass(), activeCam);
		for (auto& o : m_Objects) {
			if (o.kind != Kind::Splat || !o.splat || !o.visible) continue;
			o.splat->EncodeRender(Renderer::CurrentPass(), activeCam, viewport);
		}

		Renderer::ClosePass();
		if (perfSplat) perfSplat->ResolveAndReadTimestamps(Renderer::Encoder());

		// 3) Mesh pass — load color (preserve splats/grid), clear depth.
		bool anyMesh = false;
		for (auto& o : m_Objects) {
			if (o.kind == Kind::Mesh && o.mesh && o.visible) { anyMesh = true; break; }
		}
		if (anyMesh) {
			WGPUContext& ctx = Application::Get().GetGfx();
			EnsureDepthTexture(ctx.Width(), ctx.Height());

			glm::vec3 lDir(0.0f, 1.0f, -0.3f);
			glm::vec3 lColor(1.0f);
			glm::vec3 ambient(0.12f, 0.13f, 0.16f);
			if (const auto* lo = FirstActiveLight()) {
				const glm::vec3 d = LightWorldDirection(*lo);
				lDir = -d;
				if (lo->light) lColor = lo->light->color * lo->light->intensity;
			}

			WGPUTextureView frameView = Renderer::FrameView();
			if (frameView && m_DepthView) {
				WGPURenderPassColorAttachment color{};
				color.view       = frameView;
				color.loadOp     = WGPULoadOp_Load;
				color.storeOp    = WGPUStoreOp_Store;
				color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

				WGPURenderPassDepthStencilAttachment depth{};
				depth.view              = m_DepthView;
				depth.depthLoadOp       = WGPULoadOp_Clear;
				depth.depthStoreOp      = WGPUStoreOp_Store;
				depth.depthClearValue   = 1.0f;
				depth.depthReadOnly     = 0;
				depth.stencilLoadOp     = WGPULoadOp_Undefined;
				depth.stencilStoreOp    = WGPUStoreOp_Undefined;
				depth.stencilReadOnly   = 1;

				WGPURenderPassDescriptor rp{};
				rp.label                   = SV("editor-mesh-pass");
				rp.colorAttachmentCount    = 1;
				rp.colorAttachments        = &color;
				rp.depthStencilAttachment  = &depth;

				WGPURenderPassEncoder meshPass = wgpuCommandEncoderBeginRenderPass(
					Renderer::Encoder(), &rp);
				for (auto& o : m_Objects) {
					if (o.kind != Kind::Mesh || !o.mesh || !o.visible) continue;
					o.mesh->SetModelMatrix(BuildObjectModelMatrix(o));
					o.mesh->EncodeRender(meshPass, activeCam, viewport, lDir, lColor, ambient);
				}
				wgpuRenderPassEncoderEnd(meshPass);
				wgpuRenderPassEncoderRelease(meshPass);
			}
		}

		// 4) Gizmo overlay — load color, no depth attachment. Renders on top of
		//    meshes/splats so the transform widget is always visible/grabbable,
		//    even when the pivot lands inside a mesh (Blender/Spline/Unity do
		//    the same — pickability stays world-space; only rasterization is on top).
		BuildSceneGizmos(viewport);
		if (m_Gizmo) {
			WGPUTextureView frameView = Renderer::FrameView();
			if (frameView) {
				WGPURenderPassColorAttachment gColor{};
				gColor.view       = frameView;
				gColor.loadOp     = WGPULoadOp_Load;
				gColor.storeOp    = WGPUStoreOp_Store;
				gColor.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

				WGPURenderPassDescriptor grp{};
				grp.label                = SV("editor-gizmo-overlay");
				grp.colorAttachmentCount = 1;
				grp.colorAttachments     = &gColor;

				WGPURenderPassEncoder gizmoPass = wgpuCommandEncoderBeginRenderPass(
					Renderer::Encoder(), &grp);
				m_Gizmo->EncodeRender(gizmoPass, activeCam, viewport);
				wgpuRenderPassEncoderEnd(gizmoPass);
				wgpuRenderPassEncoderRelease(gizmoPass);
			}
		}

		Renderer::EndScene();

		if (perfSplat) {
			auto& m = perfSplat->Metrics();
			m.splatCount = static_cast<int>(perfSplat->SplatCount());
			const glm::vec3 eye = m_Camera->GetPosition();
			m.camEye[0] = eye.x; m.camEye[1] = eye.y; m.camEye[2] = eye.z;
			m.Emit();
		}

		// Throttled camera-pose broadcast for the React inspector.
		static double s_lastCamPoseEmit = 0.0;
		const double nowSec = glfwGetTime();
		if (nowSec - s_lastCamPoseEmit > 0.10) {
			s_lastCamPoseEmit = nowSec;
			const glm::vec3 pos = m_Camera->GetPosition();
			const glm::vec3 fwd = m_Camera->GetForward();
			char buf[200];
			std::snprintf(buf, sizeof(buf),
			              "{\"type\":\"editor-camera-pose\","
			              "\"position\":[%.4f,%.4f,%.4f],"
			              "\"forward\":[%.4f,%.4f,%.4f]}",
			              pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z);
			PostSceneMessage(buf);
		}
	}


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
		o.kind  = Kind::Splat;
		o.name  = AutoNameFor(StripFilename(sourceName));
		o.splat = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		o.splat->Upload(parsed);
		// Pivot = bbox bottom-center in object space. transform.position=(0,0,0)
		// then lands the object exactly on the grid with pivot at world origin.
		const auto& bb = o.splat->BoundingBox();
		if (bb.valid) {
			o.pivotObj = glm::vec3(
				(bb.min.x + bb.max.x) * 0.5f,
				bb.min.y,
				(bb.min.z + bb.max.z) * 0.5f);
		} else {
			o.pivotObj = o.splat->Centroid();
		}
		o.transform.position = glm::vec3(0.0f);
		o.splat->SetModelMatrix(BuildObjectModelMatrix(o));
		o.visible = true;

		const ObjectId id = o.id;
		m_Objects.push_back(std::move(o));
		m_Selected = id;

		INFO_CORE("EditorScene: loaded splat id={0} ('{1}'), total objects={2}",
		          (uint64_t)id, FindObject(id)->name, (uint64_t)m_Objects.size());

		PostObjectsList();
		PostSelectionChanged();
		PostTransformUpdate(true);
		return id;
	}


	EditorScene::ObjectId EditorScene::LoadMeshFromBytes(const uint8_t* data, size_t size,
	                                                     const std::string& sourceName)
	{
		INFO_CORE("EditorScene: parsing {0} byte glb payload (name='{1}')",
		          (uint64_t)size, sourceName);
		MeshData parsed = GltfLoader::LoadGlbFromBytes(data, size, sourceName.c_str());
		if (parsed.Empty()) {
			ERROR_CORE("EditorScene: glb parse returned no primitives");
			PostSceneMessage("{\"type\":\"editor-error\",\"message\":\"Failed to parse glb\"}");
			return 0;
		}

		EditorObject o;
		o.id    = m_NextId++;
		o.kind  = Kind::Mesh;
		o.name  = AutoNameFor(StripFilename(sourceName));
		o.mesh  = std::make_unique<MeshRenderer>(Application::Get().GetGfx());
		o.mesh->Upload(parsed);
		// Land the mesh's AABB-bottom on the grid.
		if (parsed.aabbValid) {
			o.transform.position.y = -parsed.aabbMin.y;
		}
		o.mesh->SetModelMatrix(o.transform.Matrix());
		o.visible = true;

		const ObjectId id = o.id;
		m_Objects.push_back(std::move(o));
		m_Selected = id;

		INFO_CORE("EditorScene: loaded mesh id={0} ('{1}'), total objects={2}",
		          (uint64_t)id, FindObject(id)->name, (uint64_t)m_Objects.size());

		PostObjectsList();
		PostSelectionChanged();
		PostTransformUpdate(true);
		return id;
	}


	EditorScene::ObjectId EditorScene::AddLight(const std::string& kind)
	{
		(void)kind; // v1: only directional supported
		EditorObject o;
		o.id    = m_NextId++;
		o.kind  = Kind::Light;
		o.name  = AutoNameFor("Light");
		o.transform.position = glm::vec3(0.0f, 3.0f, 0.0f);
		o.light = LightProps{};
		o.visible = true;

		const ObjectId id = o.id;
		m_Objects.push_back(std::move(o));
		m_Selected = id;

		PostObjectsList();
		PostSelectionChanged();
		PostTransformUpdate(true);
		return id;
	}


	void EditorScene::SetLightProps(ObjectId id, const glm::vec3& color, float intensity)
	{
		auto* o = FindObject(id);
		if (!o || o->kind != Kind::Light || !o->light) return;
		o->light->color = color;
		o->light->intensity = std::max(0.0f, intensity);
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
		if (id != 0 && !FindObject(id)) return;
		if (m_Selected == id) return;
		m_Selected = id;
		PostSelectionChanged();
		PostTransformUpdate(true);
	}


	void EditorScene::FocusObject(ObjectId id)
	{
		const auto* o = FindObject(id);
		if (!o) return;
		glm::vec3 mn, mx;
		ComputeWorldAabb(*o, mn, mx);
		const glm::vec3 center = (mn + mx) * 0.5f;
		const float diag = std::max(0.5f, glm::length(mx - mn));
		const glm::vec3 fwd = m_Camera->GetForward();
		const glm::vec3 eye = center - fwd * (diag * 1.6f);
		SwitchCameraToFly(eye, glm::normalize(center - eye));
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
		if (o->splat) o->splat->SetModelMatrix(BuildObjectModelMatrix(*o));
		if (o->mesh)  o->mesh->SetModelMatrix(BuildObjectModelMatrix(*o));
		PostTransformUpdate(true);
	}


	void EditorScene::SetCameraPose(const glm::vec3& pos, const glm::vec3& fwd)
	{
		SwitchCameraToFly(pos, fwd);
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


	void EditorScene::PostObjectsList()
	{
		std::string body = "{\"type\":\"editor-objects\",\"objects\":[";
		bool first = true;
		for (const auto& o : m_Objects) {
			const char* kindStr =
				o.kind == Kind::Splat ? "splat" :
				o.kind == Kind::Mesh  ? "mesh"  :
				"light_directional";
			uint64_t count = 0;
			if (o.kind == Kind::Splat && o.splat) count = o.splat->SplatCount();
			else if (o.kind == Kind::Mesh && o.mesh) count = o.mesh->PrimitiveCount();

			char row[384];
			if (o.kind == Kind::Light && o.light) {
				std::snprintf(row, sizeof(row),
				              "%s{\"id\":%llu,\"kind\":\"%s\",\"name\":\"%s\",\"visible\":%s,"
				              "\"count\":%llu,"
				              "\"light\":{\"color\":[%.3f,%.3f,%.3f],\"intensity\":%.3f}}",
				              first ? "" : ",",
				              (unsigned long long)o.id, kindStr, o.name.c_str(),
				              o.visible ? "true" : "false",
				              (unsigned long long)count,
				              o.light->color.r, o.light->color.g, o.light->color.b,
				              o.light->intensity);
			} else {
				std::snprintf(row, sizeof(row),
				              "%s{\"id\":%llu,\"kind\":\"%s\",\"name\":\"%s\",\"visible\":%s,\"count\":%llu}",
				              first ? "" : ",",
				              (unsigned long long)o.id, kindStr, o.name.c_str(),
				              o.visible ? "true" : "false",
				              (unsigned long long)count);
			}
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

EDITOR_EXPORT void editor_load_mesh_bytes(uint8_t* data, int len, const char* name)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	if (!data || len <= 0) return;
	s->LoadMeshFromBytes(data, static_cast<size_t>(len), name ? name : "");
}

EDITOR_EXPORT void editor_add_light(const char* kind)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->AddLight(kind ? kind : "directional");
}

EDITOR_EXPORT void editor_set_light_props(double id, double r, double g, double b, double intensity)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	s->SetLightProps(static_cast<::Sandbox::EditorScene::ObjectId>(id),
	                 glm::vec3((float)r, (float)g, (float)b), (float)intensity);
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

EDITOR_EXPORT void editor_set_camera_pose(double px, double py, double pz,
                                          double fx, double fy, double fz)
{
	auto* s = ::Sandbox::EditorScene::Current();
	if (!s) return;
	const glm::vec3 fwd((float)fx, (float)fy, (float)fz);
	const glm::vec3 pos((float)px, (float)py, (float)pz);
	if (glm::length(fwd) < 1e-4f) return;
	s->SetCameraPose(pos, glm::normalize(fwd));
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
