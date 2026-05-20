#include "GaussianSplatPlayerScene.h"

#include "SceneRegistry.h"
#include "../SceneSelector.h"
#include "Engine/Application.h"
#include "Engine/Input.h"
#include "Engine/KeyCodes.h"
#include "Engine/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SplatLoader.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

using namespace Engine;

namespace Sandbox {

	namespace {

		// Tiny JSON value extractor — fixed manifest schema, no need for a dep.
		std::optional<std::string> JsonGetString(const std::string& body, const char* key) {
			std::string needle = std::string("\"") + key + "\"";
			size_t k = body.find(needle);
			if (k == std::string::npos) return std::nullopt;
			size_t colon = body.find(':', k);
			if (colon == std::string::npos) return std::nullopt;
			size_t q1 = body.find('"', colon);
			if (q1 == std::string::npos) return std::nullopt;
			size_t q2 = body.find('"', q1 + 1);
			if (q2 == std::string::npos) return std::nullopt;
			return body.substr(q1 + 1, q2 - q1 - 1);
		}

		std::optional<double> JsonGetNumber(const std::string& body, const char* key) {
			std::string needle = std::string("\"") + key + "\"";
			size_t k = body.find(needle);
			if (k == std::string::npos) return std::nullopt;
			size_t colon = body.find(':', k);
			if (colon == std::string::npos) return std::nullopt;
			size_t p = colon + 1;
			while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
			size_t end = p;
			while (end < body.size() &&
			       (std::isdigit((unsigned char)body[end]) || body[end] == '.' ||
			        body[end] == '-' || body[end] == 'e' || body[end] == 'E' ||
			        body[end] == '+'))
			{
				++end;
			}
			if (end == p) return std::nullopt;
			try {
				return std::stod(body.substr(p, end - p));
			} catch (...) {
				return std::nullopt;
			}
		}

	} // namespace

	GaussianSplatPlayerScene::Manifest
	GaussianSplatPlayerScene::LoadManifest(const std::string& dir) const
	{
		namespace fs = std::filesystem;
		Manifest m;
		m.fps = 24.0f;

		// 1) Try to read manifest.json — overrides defaults if present.
		std::string prefix = "frame_";
		int pad = 5;
		int frameCount = 0;
		{
			fs::path mp = fs::path(dir) / "manifest.json";
			std::ifstream f(mp);
			if (f) {
				std::stringstream ss; ss << f.rdbuf();
				const std::string body = ss.str();
				if (auto v = JsonGetNumber(body, "fps"); v) m.fps = (float)*v;
				if (auto v = JsonGetNumber(body, "frame_count"); v) frameCount = (int)*v;
				if (auto v = JsonGetString(body, "prefix"); v) prefix = *v;
				if (auto v = JsonGetNumber(body, "pad"); v) pad = (int)*v;
			}
		}

		// Manifest path: build the list directly, skip the directory scan.
		if (frameCount > 0) {
			for (int i = 0; i < frameCount; ++i) {
				char idx[32];
				std::snprintf(idx, sizeof(idx), "%0*d", pad, i);
				fs::path p = fs::path(dir) / (prefix + idx + ".splat");
				m.framePaths.push_back(p.string());
			}
			return m;
		}

		// Fallback: glob *.splat in lexicographic order.
		std::vector<std::string> found;
		std::error_code ec;
		for (auto& e : fs::directory_iterator(dir, ec)) {
			if (!e.is_regular_file()) continue;
			const auto& p = e.path();
			if (p.extension() != ".splat") continue;
			found.push_back(p.string());
		}
		std::sort(found.begin(), found.end());
		m.framePaths = std::move(found);
		return m;
	}

	GaussianSplatPlayerScene::GaussianSplatPlayerScene(float screenWidth, float screenHeight)
		: SceneBase("gsplat_player", screenWidth, screenHeight)
	{
		const float aspect = m_ScreenWidth / m_ScreenHeight;
		m_OrbitCam.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 10000.0f);
		m_FlyCam  .SetPerspective(glm::radians(45.0f), aspect, 0.1f, 10000.0f);

		m_FlyCam.m_MaxMoveSpeed = 20.0f;  // 5× default, ml-sharp scenes are large

		auto dirParam = ReadParam("player_dir");
		if (!dirParam) {
			ERROR_CORE("gsplat_player: --player_dir=<folder> is required");
			return;
		}
		const std::string dir = *dirParam;

		m_Manifest = LoadManifest(dir);
		if (m_Manifest.framePaths.empty()) {
			ERROR_CORE("gsplat_player: no .splat files found in '{0}'", dir);
			return;
		}
		INFO_CORE("gsplat_player: {0} frames at {1} fps from '{2}'",
		          (uint64_t)m_Manifest.framePaths.size(),
		          m_Manifest.fps,
		          dir);

		// Optional overrides — let the user re-pin fps from the CLI without
		// rewriting the manifest, or disable looping.
		if (auto v = ReadParam("player_fps"); v) {
			try { m_Manifest.fps = std::stof(*v); } catch (...) {}
		}
		if (auto v = ReadParam("player_loop"); v) {
			m_Loop = (*v != "0" && *v != "false");
		}

		// Frame 0 sync — need something to render + centroid for orbit pivot.
		SplatData first = SplatLoader::LoadSplat(m_Manifest.framePaths[0]);
		if (first.Empty()) {
			ERROR_CORE("gsplat_player: failed to load first frame '{0}'",
			           m_Manifest.framePaths[0]);
			return;
		}
		m_SplatCount = first.Count();

		// Auto-frame: alpha-filtered centroid + 95-pct radius. Pivot is
		// fixed for the whole sequence to avoid lurching on per-frame drift.
		glm::vec3 target(0.0f);
		float     spawnRadius = 1.5f;
		{
			std::vector<glm::vec3> kept;
			kept.reserve(first.positions.size() / 2);
			for (size_t i = 0; i < first.positions.size(); ++i) {
				if (first.colors[i].a >= 32) kept.push_back(first.positions[i]);
			}
			if (kept.empty()) kept.assign(first.positions.begin(), first.positions.end());

			glm::dvec3 sum(0.0);
			for (const auto& p : kept) sum += glm::dvec3(p);
			target = glm::vec3(sum / (double)kept.size());

			// 95-pct per-axis half-extent → vector-norm gives a robust radius.
			auto pctAxis = [&](int axis, float pct) {
				std::vector<float> v; v.reserve(kept.size());
				for (const auto& p : kept) v.push_back(std::fabs(p[axis] - target[axis]));
				std::sort(v.begin(), v.end());
				size_t k = std::min(v.size() - 1, (size_t)((pct / 100.0f) * v.size()));
				return v[k];
			};
			const float hx = pctAxis(0, 95.0f);
			const float hy = pctAxis(1, 95.0f);
			const float hz = pctAxis(2, 95.0f);
			float r = std::sqrt(hx * hx + hy * hy + hz * hz);
			r = std::clamp(r, 0.5f, 8.0f);
			spawnRadius = r;
		}

		// +z pullback (loader flips subject to -z). 1.8× radius frames it.
		glm::vec3 eye = target + glm::vec3(0.0f, 0.0f, spawnRadius * 1.8f);
		if (auto s = ReadParam("player_eye"); s) {
			if (auto v = ParseVec3(*s); v) eye = *v;
		}
		if (auto s = ReadParam("player_spawn_radius"); s) {
			try {
				float r = std::stof(*s);
				eye = target + glm::vec3(0.0f, 0.0f, r);
			} catch (...) {}
		}
		INFO_CORE("gsplat_player: target=({0:.2f},{1:.2f},{2:.2f}) radius={3:.2f} eye=({4:.2f},{5:.2f},{6:.2f})",
		          target.x, target.y, target.z, spawnRadius, eye.x, eye.y, eye.z);
		m_OrbitCam.SetOrbit(target, eye);

		// Renderer reused across frames via further Upload() calls.
		m_Splats = std::make_unique<GaussianSplatRenderer>(Application::Get().GetGfx());
		m_Splats->Upload(first);
		m_CurrentFrame = 0;
		m_PlaybackT0 = -1.0;  // first OnUpdate captures the t0

		// Start the background decoder from frame 1 — frame 0 is already
		// on the GPU, no point re-decoding it.
		m_NextDecodeIndex.store(1);
		StartDecoder();
	}

	GaussianSplatPlayerScene::~GaussianSplatPlayerScene()
	{
		StopDecoder();
	}

	void GaussianSplatPlayerScene::StartDecoder()
	{
		m_DecoderStop.store(false);
		m_DecoderThread = std::thread([this]() { DecoderLoop(); });
	}

	void GaussianSplatPlayerScene::StopDecoder()
	{
		if (!m_DecoderThread.joinable()) return;
		{
			std::lock_guard<std::mutex> lk(m_QueueMu);
			m_DecoderStop.store(true);
		}
		m_QueueNotFull.notify_all();
		m_QueueNotEmpty.notify_all();
		m_DecoderThread.join();
	}

	void GaussianSplatPlayerScene::DecoderLoop()
	{
		// One I/O + parse thread is enough: SplatLoader::LoadSplat is
		// mostly raw-file read + a tight per-record copy loop, so it
		// saturates a single core fine. Spinning multiple decoders
		// would also fight the main thread's GPU upload (which itself
		// holds the WGPU queue lock).
		const int total = (int)m_Manifest.framePaths.size();
		while (!m_DecoderStop.load()) {
			int idx = m_NextDecodeIndex.fetch_add(1);
			if (idx >= total) {
				if (!m_Loop) break;
				// Loop point: rewind. The consumer is responsible for
				// not double-incrementing past this — see OnUpdate.
				m_NextDecodeIndex.store(0);
				continue;
			}
			SplatData data = SplatLoader::LoadSplat(m_Manifest.framePaths[idx]);

			std::unique_lock<std::mutex> lk(m_QueueMu);
			m_QueueNotFull.wait(lk, [this]() {
				return m_DecodedQueue.size() < (size_t)kQueueMaxFrames ||
				       m_DecoderStop.load();
			});
			if (m_DecoderStop.load()) return;
			Frame f;
			f.index = idx;
			f.data  = std::move(data);
			m_DecodedQueue.push_back(std::move(f));
			lk.unlock();
			m_QueueNotEmpty.notify_one();
		}
	}

	void GaussianSplatPlayerScene::OnUpdate(Timestep ts)
	{
		(void)ts;
		if (!m_Splats || m_Manifest.framePaths.empty()) return;

		// --- Camera mode arbitration ---------------------------------------
		// First WASDEQ press promotes orbit → fly (same UX as the static
		// gsplat scene). Tab (edge-triggered) brings you back to orbit.
		const bool tabDown = Input::IsKeyPressed(KEY_TAB);
		if (tabDown && !m_PrevTabDown) {
			SetMode(CameraMode::Orbit);
		} else if (m_Mode == CameraMode::Orbit && AnyFlyKeyPressed()) {
			SetMode(CameraMode::Fly);
		}
		m_PrevTabDown = tabDown;

		if (m_Mode == CameraMode::Orbit) {
			m_OrbitCam.Update(ts);
		} else {
			m_FlyCam.Update(ts);
		}
		const SPtr<Camera> activeRenderCam = (m_Mode == CameraMode::Orbit)
		    ? m_OrbitCam.GetRenderCamera()
		    : m_FlyCam  .GetRenderCamera();

		// --- Playback scheduling -------------------------------------------
		// At fps F, the target frame index for wall-clock t is floor((t - t0) * F).
		// We may advance multiple frames per tick if the main thread is
		// behind (e.g. after a stall) — but only as long as the decoded
		// queue has the frames ready. If not, we hold on the current
		// frame instead of doing a synchronous load (which would stall
		// the GPU pipeline harder than the visible hitch).
		const double now = glfwGetTime();
		if (m_PlaybackT0 < 0.0) m_PlaybackT0 = now;
		const double fps    = std::max(1.0, (double)m_Manifest.fps);
		const int    target = (int)((now - m_PlaybackT0) * fps);

		const int total = (int)m_Manifest.framePaths.size();
		int desired = target;
		if (m_Loop) {
			desired = ((desired % total) + total) % total;
		} else {
			desired = std::min(desired, total - 1);
		}

		// Pull frames out of the queue until we land on the desired index.
		// Drops intermediate frames on purpose: the player has to keep
		// up with wall-clock fps, not visit every produced frame, when
		// playback rate exceeds the decoder throughput.
		if (desired != m_CurrentFrame) {
			std::unique_lock<std::mutex> lk(m_QueueMu);
			Frame chosen;
			bool have = false;
			while (!m_DecodedQueue.empty()) {
				Frame& f = m_DecodedQueue.front();
				if (f.index == desired) {
					chosen = std::move(f);
					m_DecodedQueue.pop_front();
					have = true;
					break;
				}
				// Older frame — drop. Newer-than-desired (possible after a
				// loop rewind) — also drop here; the decoder will refill
				// at the right index after the consumer catches up.
				m_DecodedQueue.pop_front();
			}
			lk.unlock();
			m_QueueNotFull.notify_all();

			if (have && !chosen.data.Empty()) {
				m_SplatCount = chosen.data.Count();
				m_Splats->Upload(chosen.data);
				m_CurrentFrame = desired;
			}
			// If we didn't have the desired frame staged, we just keep
			// rendering the previous one — no synchronous IO on the
			// hot path.
		}

		// --- Render pass (mirrors GaussianSplatScene's structure) ----------
		const double frameStart = glfwGetTime();
		if (m_PrevFrameStart > 0.0) {
			m_Splats->Metrics().frameMs.Push(
				static_cast<float>((frameStart - m_PrevFrameStart) * 1000.0));
		}
		m_PrevFrameStart = frameStart;

		// Drain async timestamp readbacks (perf overlay only — same as gsplat).
		m_Splats->TickPerf();

		if (!Renderer::BeginScene(activeRenderCam)) return;

		const double encodeStart = glfwGetTime();
		const glm::mat4& view = activeRenderCam->GetViewMatrix();
		const glm::mat4& proj = activeRenderCam->GetProjectionMatrix();
		m_Splats->EncodeSort(Renderer::Encoder(), view, proj);

		const WGPUPassTimestampWrites* renderTw = m_Splats->GetRenderPassTimestampWrites();
		Renderer::OpenColorPass(0.05f, 0.05f, 0.08f, 1.0f, renderTw);

		m_Splats->EncodeRender(Renderer::CurrentPass(),
		                       activeRenderCam,
		                       glm::vec2(m_ScreenWidth, m_ScreenHeight));

		Renderer::ClosePass();
		m_Splats->ResolveAndReadTimestamps(Renderer::Encoder());

		const double encodeEnd = glfwGetTime();
		auto& mm = m_Splats->Metrics();
		mm.cpuEncodeMs.Push(static_cast<float>((encodeEnd - encodeStart) * 1000.0));
		mm.splatCount = static_cast<int>(m_SplatCount);
		const glm::vec3 eye = (m_Mode == CameraMode::Orbit)
		    ? m_OrbitCam.GetPosition()
		    : m_FlyCam  .GetPosition();
		mm.camEye[0] = eye.x; mm.camEye[1] = eye.y; mm.camEye[2] = eye.z;
		mm.Emit();

		++m_FpsCounter;
		++m_FrameDrawCount;
		if (m_FpsT0 == 0.0) m_FpsT0 = now;
		if (m_FpsCounter % 60 == 0) {
			float dt = (float)(now - m_FpsT0);
			INFO_CORE("gsplat_player: 60 frames in {0:.3f}s = {1:.1f} fps  (playback frame {2}/{3})",
			          dt, 60.0f / dt, m_CurrentFrame, total);
			m_FpsT0 = now;
		}

		Renderer::EndScene();
	}

	bool GaussianSplatPlayerScene::AnyFlyKeyPressed() const
	{
		return Input::IsKeyPressed(KEY_W) || Input::IsKeyPressed(KEY_A)
		    || Input::IsKeyPressed(KEY_S) || Input::IsKeyPressed(KEY_D)
		    || Input::IsKeyPressed(KEY_Q) || Input::IsKeyPressed(KEY_E);
	}


	void GaussianSplatPlayerScene::SetMode(CameraMode mode)
	{
		if (mode == m_Mode) return;

		// No view jump on either transition (same logic as the static
		// gsplat scene). Orbit → fly: drop fly cam onto orbit pose. Fly
		// → orbit: re-pivot orbit at the previous orbit radius in front
		// of the fly camera so the user orbits around whatever they
		// were aiming at, not the auto-framed centroid.
		if (mode == CameraMode::Fly) {
			const glm::vec3 eye = m_OrbitCam.GetPosition();
			m_FlyCam.SetPose(eye, m_OrbitCam.GetTarget() - eye);
		} else {
			const glm::vec3 eye    = m_FlyCam.GetPosition();
			const glm::vec3 fwd    = m_FlyCam.GetForward();
			const float     radius = m_OrbitCam.GetRadius();
			m_OrbitCam.SetOrbit(eye + fwd * radius, eye);
		}
		m_Mode = mode;
		INFO_CORE("gsplat_player: camera mode → {0}",
		          mode == CameraMode::Orbit ? "orbit" : "fly");
	}


	SCENE_REGISTER("gsplat_player", GaussianSplatPlayerScene)

}
