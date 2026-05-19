#include "GaussianSplatPlayerScene.h"

#include "SceneRegistry.h"
#include "../SceneSelector.h"
#include "Engine/Application.h"
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

		// Minimal hand-rolled JSON value extractor for the player manifest.
		// We deliberately avoid pulling a JSON dep into the engine for one
		// small file — the schema is fixed (sharp-video-local writes it) and
		// we only need three keys: "fps", "frame_count", "prefix" / "pad" are
		// optional. Returns std::nullopt on parse miss, leaving the caller
		// to fall back to defaults + a directory glob.
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

		// 2) If manifest told us the exact frame count + naming pattern,
		// construct the path list directly — much faster than directory_iterator
		// on a folder with thousands of files.
		if (frameCount > 0) {
			for (int i = 0; i < frameCount; ++i) {
				char idx[32];
				std::snprintf(idx, sizeof(idx), "%0*d", pad, i);
				fs::path p = fs::path(dir) / (prefix + idx + ".splat");
				m.framePaths.push_back(p.string());
			}
			return m;
		}

		// 3) Fall back to globbing every .splat in lexicographic order.
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

		// --- Required: --player_dir=<path> ----------------------------------
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

		// --- Load frame 0 synchronously so we have something to render
		// immediately (and so we can centre the orbit pivot on its centroid).
		SplatData first = SplatLoader::LoadSplat(m_Manifest.framePaths[0]);
		if (first.Empty()) {
			ERROR_CORE("gsplat_player: failed to load first frame '{0}'",
			           m_Manifest.framePaths[0]);
			return;
		}
		m_SplatCount = first.Count();

		// Auto-frame on the first frame's centroid (alpha-filtered, same
		// threshold as GaussianSplatScene). The orbit pivot stays fixed
		// across the whole sequence so the camera doesn't lurch when
		// ml-sharp's per-frame mean drifts.
		glm::vec3 target(0.0f);
		{
			glm::dvec3 sum(0.0);
			size_t kept = 0;
			for (size_t i = 0; i < first.positions.size(); ++i) {
				if (first.colors[i].a >= 32) {
					sum += glm::dvec3(first.positions[i]);
					++kept;
				}
			}
			if (kept == 0) {
				for (const auto& p : first.positions) sum += glm::dvec3(p);
				kept = std::max<size_t>(1, first.positions.size());
			}
			target = glm::vec3(sum / static_cast<double>(kept));
		}

		// Default-ish spawn — same heuristic as the static gsplat scene's
		// kDefaultSpawn. CLI override via --player_eye=x,y,z.
		glm::vec3 eye = target + glm::vec3(0.0f, 0.0f, 1.5f);
		if (auto s = ReadParam("player_eye"); s) {
			if (auto v = ParseVec3(*s); v) eye = *v;
		}
		m_OrbitCam.SetOrbit(target, eye);

		// Upload frame 0 to the GPU. The renderer object is reused for
		// every subsequent frame via additional Upload() calls; bind
		// groups + sort scratch are reallocated inside Upload() each
		// time, which Dawn handles cleanly.
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

		// --- Camera (orbit-only for the player) -----------------------------
		m_OrbitCam.Update(ts);
		const SPtr<Camera> activeRenderCam = m_OrbitCam.GetRenderCamera();

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
		const glm::vec3 eye = m_OrbitCam.GetPosition();
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

	SCENE_REGISTER("gsplat_player", GaussianSplatPlayerScene)

}
