#pragma once

#include "SceneBase.h"
#include "Engine/OrbitCamera.h"
#include "Engine/Renderer/GaussianSplatRenderer.h"
#include "Engine/Renderer/SplatLoader.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Sandbox {

	/*
	 * GaussianSplatPlayerScene
	 *
	 * Plays a folder of .splat files back as a "gaussian video": the
	 * GaussianSplatRenderer is re-Upload'd from one frame's SplatData
	 * to the next at the manifest's playback fps. Pre-decoded frames
	 * are produced by a background thread into a bounded ring so the
	 * main thread never blocks on disk + parse during playback.
	 *
	 * Selected via:
	 *     --scene=gsplat_player --player_dir=<absolute path>
	 *
	 * The folder layout matches what services/sharp-video-local writes:
	 *   <dir>/manifest.json       (optional; fps + frame_count)
	 *   <dir>/frame_00001.splat
	 *   <dir>/frame_00002.splat
	 *    ...
	 * If manifest.json is missing or unparseable, falls back to globbing
	 * <dir>/*.splat in lexicographic order at the default fps.
	 *
	 * Camera: orbit-only. The pivot is locked to the FIRST frame's
	 * centroid for the whole playback — re-pivoting per frame would
	 * jitter the view as ml-sharp's per-frame centroids drift.
	 *
	 * NOTE: This scene deliberately reuses GaussianSplatRenderer
	 * unchanged. All player logic (file discovery, background decode,
	 * frame scheduling, hot-swap) lives here so the renderer stays a
	 * pure single-scene drawer.
	 */
	class GaussianSplatPlayerScene final : public SceneBase
	{
	public:
		GaussianSplatPlayerScene(float screenWidth, float screenHeight);
		~GaussianSplatPlayerScene() override;

		void OnUpdate(Engine::Timestep ts) override;

	private:
		struct Frame {
			int                 index = -1;
			Engine::SplatData   data;
		};

		// Folder + playback metadata, read once at construction.
		struct Manifest {
			float                   fps         = 24.0f;
			std::vector<std::string> framePaths;  // absolute, lexicographically sorted
		};

		Manifest LoadManifest(const std::string& dir) const;

		// Background decoder lifecycle. Producer pulls indices from
		// m_NextDecodeIndex (atomic) and pushes parsed frames into
		// m_DecodedQueue up to kQueueMaxFrames deep.
		void DecoderLoop();
		void StartDecoder();
		void StopDecoder();

		Engine::OrbitCamera m_OrbitCam;
		std::unique_ptr<Engine::GaussianSplatRenderer> m_Splats;

		Manifest                                      m_Manifest;

		// Playback state.
		int    m_CurrentFrame = 0;   // index of frame currently uploaded to GPU
		double m_PlaybackT0   = 0.0; // glfwGetTime() at first frame; -1 until set
		size_t m_SplatCount   = 0;
		int    m_FrameDrawCount = 0;
		int    m_FpsCounter     = 0;
		double m_FpsT0          = 0.0;
		double m_PrevFrameStart = 0.0;  // wall-clock between scene ticks, for frameMs metric

		// Producer/consumer queue of pre-decoded frames.
		static constexpr int kQueueMaxFrames = 8;
		std::mutex                  m_QueueMu;
		std::condition_variable     m_QueueNotFull;
		std::condition_variable     m_QueueNotEmpty;
		std::deque<Frame>           m_DecodedQueue;
		std::atomic<int>            m_NextDecodeIndex{0};
		std::atomic<bool>           m_DecoderStop{false};
		std::thread                 m_DecoderThread;

		bool                        m_Loop = true;
	};

}
