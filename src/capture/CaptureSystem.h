#pragma once

#include "CaptureTypes.h"

#include "Device.h"
#include "Allocator.h"
#include "CommandBuffer.h"
#include "CommandPool.h"
#include "Buffer.h"
#include "Image.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VideoEncoder;
class StillEncoder;

namespace Capture {

	// =============================================================================
	// CaptureSystem
	// =============================================================================
	//
	// Owns the screenshot + recording pipeline. Lives on RenderingSystem.
	//
	// Per-frame integration is one call: RenderingSystem::DrawFrame invokes
	// OnRenderRecord(cmd, frameIndex, image, layout, format, extent) AFTER the
	// graph has executed (so the post-process output is materialized) but BEFORE
	// the command buffer is ended. If a screenshot is queued or recording is
	// active, this records:
	//
	//     vkCmdResetEvent(slot.event, ...)
	//     vkCmdTransitionImageLayout(image, ... -> TRANSFER_SRC)
	//     vkCmdCopyImageToBuffer(image, slot.staging)
	//     vkCmdTransitionImageLayout(image, TRANSFER_SRC -> ...)
	//     vkCmdSetEvent(slot.event, ...)
	//
	// into the same command buffer. No vkDeviceWaitIdle. No extra queue submit.
	//
	// A worker thread observes when each slot's GPU work has completed via a
	// monotonic frame counter. Each slot is tagged with the frame number at
	// which its copy was recorded; the worker treats it as GPU-done once the
	// counter has advanced by `maxFramesInFlight` (at which point the
	// FrameController has waited on that frame's in-flight fence again, which
	// guarantees the prior submit completed).
	//
	// Why a frame counter and not VkEvent or per-frame fences:
	//   - VkEvent isn't supported under VK_KHR_portability_subset (MoltenVK on
	//     macOS), which the engine targets.
	//   - The FrameController owns the per-frame fences and reuses them across
	//     the maxFramesInFlight ring; a per-slot capture straddling a fence
	//     reset would race. The frame counter is owned by us and monotonic.
	//   - The latency cost is exactly maxFramesInFlight frames (~33ms at 60fps,
	//     2 frames-in-flight) — invisible to the user; matches the GPU's own
	//     pipelining latency.
	struct CaptureSystemConfig {
		std::shared_ptr<VWrap::Device>      device;
		std::shared_ptr<VWrap::Allocator>   allocator;
		std::shared_ptr<VWrap::CommandPool> graphicsCommandPool;  // (kept for parity; copy commands ride the main frame buffer)
		uint32_t                            maxFramesInFlight = 2;
		std::filesystem::path               screenshotsDir = "screenshots";
		std::filesystem::path               recordingsDir  = "recordings";
	};

	class CaptureSystem {
	public:
		// Constructor + destructor defined in .cpp so the unique_ptr<StillEncoder>
		// / unique_ptr<VideoEncoder> members can use forward-declared types here
		// — keeps ffmpeg headers out of every TU that pulls in CaptureSystem.h.
		CaptureSystem();
		~CaptureSystem();
		CaptureSystem(const CaptureSystem&) = delete;
		CaptureSystem& operator=(const CaptureSystem&) = delete;

		// Lifecycle. Init allocates the ring up to the per-frame count + headroom;
		// the actual staging buffers are sized lazily on first use (extent isn't
		// known until the first capture) and re-sized on extent changes.
		void Init(const CaptureSystemConfig& cfg);
		void Shutdown();

		// ---- One-shot screenshot ----
		// Marks "capture next frame into the ring as a PNG screenshot". Idempotent
		// per-frame (multiple calls between OnRenderRecord invocations collapse
		// to one capture).
		void RequestScreenshot();

		// ---- Recording lifecycle ----
		// Toggle: if idle → start (calls BeginRecording on next OnRenderRecord
		// when a slot is available); if recording → stop (waits for ring drain
		// and finalizes the MP4 in EndRecording).
		void ToggleRecording();
		bool IsRecording() const { return m_recording.load(std::memory_order_acquire); }

		// ---- Per-frame hook (called from RenderingSystem::DrawFrame) ----
		// Records copy commands into `cmd` if there's pending capture work for
		// this frame. The image / layout / format / extent describe the source
		// (typically the post-process chain's final scene image). Layout describes
		// the image's current layout — we transition to TRANSFER_SRC and back.
		void OnRenderRecord(
			std::shared_ptr<VWrap::CommandBuffer> cmd,
			uint32_t                              frameIndex,
			std::shared_ptr<VWrap::Image>         image,
			VkImageLayout                         currentLayout,
			VkFormat                              format,
			VkExtent2D                            extent);

		// ---- UI / Application wiring ----
		RecordingOptions& GetOptions()              { return m_options; }
		const RecordingOptions& GetOptions() const  { return m_options; }
		RecordingStatus   GetStatus() const;
		std::string       GetLastScreenshotPath() const;
		std::string       GetLastRecordingPath()  const;

		// Optional callbacks fire from the worker thread. Application marshals
		// them back to the main thread via the RenderingSystem event queue if
		// it cares; current usage just stores the path on the inspector.
		void SetOnScreenshotSaved(std::function<void(std::string)> cb) { m_onScreenshotSaved = std::move(cb); }
		void SetOnRecordingSaved (std::function<void(std::string)> cb) { m_onRecordingSaved  = std::move(cb); }

		// FixedStep helper: returns the logical time elapsed since recording
		// started, advancing by 1/targetFps per captured frame. Drives Phase-7
		// time plumbing — when not recording or in RealTime mode, returns
		// wall-clock time since process start so techniques behave normally.
		// Thread-safe (reads atomics).
		double GetLogicalTimeSeconds() const;

	private:
		// ---- Ring slot ----
		// One slot per concurrent in-flight capture. Pre-allocated; staging
		// buffer is created lazily and re-created on extent / format change.
		enum class SlotState : uint32_t {
			Free        = 0,   // owned by no one; pool can hand it out
			GpuPending  = 1,   // copy recorded; waiting on event
			WorkerOwned = 2,   // worker is processing pixels
		};

		// Tags the work the slot represents. Set when the slot is checked out.
		enum class JobKind : uint8_t {
			Screenshot,
			RecordingFrame,
		};

		// Note: std::atomic is neither copy- nor move-constructible, so Slot is
		// neither either. We hold the ring as unique_ptr<Slot[]> + size so the
		// container never relocates elements — std::vector::resize would.
		struct Slot {
			std::shared_ptr<VWrap::Buffer> staging;
			VkDeviceSize                   stagingSize     = 0;
			std::atomic<SlotState>         state{SlotState::Free};
			uint64_t                       recordedAtFrame = 0;  // monotonic frame # when copy was recorded
			JobKind                        kind            = JobKind::Screenshot;
			VkExtent2D                     extent          = {0, 0};
			VkFormat                       format          = VK_FORMAT_UNDEFINED;
			bool                           swizzleBgra     = false;
			int64_t                        videoPts        = 0;   // for RecordingFrame
		};

		// Pick a free slot, sizing/recreating its staging buffer if the extent
		// changed. Returns nullptr if none available; caller decides whether to
		// drop or block (per BackPressure). On Block, this calls vkGetEventStatus
		// in a tight loop on the oldest pending slot.
		Slot* AcquireSlot(VkExtent2D extent, VkFormat format,
		                  bool isRecordingFrame, BackPressure backPressure);

		// Worker thread entry — drains processed slots until shutdown.
		void  WorkerLoop();

		// Process one slot (worker side). Maps staging, swizzles BGRA→RGBA in
		// place if needed, dispatches to PNG or VideoEncoder, marks Free.
		void  ProcessSlot(Slot& slot);

		// Open/close the ffmpeg encoder. Called from the worker thread when the
		// first recording frame is processed (Open) and from EndRecording (Close).
		void  OpenVideoEncoder(VkExtent2D extent);
		void  CloseVideoEncoder();

		// Block until every slot is Free. Used by EndRecording to ensure the
		// encoder has consumed every queued frame before we close the file.
		void  DrainRing();

		CaptureSystemConfig m_cfg{};
		RecordingOptions    m_options{};       // user-tunable; surfaced via GetOptions()

		std::unique_ptr<Slot[]> m_ring;
		size_t                  m_ringSize = 0;
		std::mutex              m_slotMutex;     // guards slot pool checkout / ring resizing
		std::condition_variable m_workerCv;      // worker waits on this
		std::atomic<bool>  m_workerExit{false};
		std::thread        m_worker;

		// Monotonic frame counter — bumped at the top of every OnRenderRecord
		// call. Worker uses (current - slot.recordedAtFrame) >= maxFramesInFlight
		// to decide a slot's GPU work has completed. m_drainFlag bypasses the
		// check during DrainRing (after vkDeviceWaitIdle, all slots are GPU-done
		// regardless of where the counter sits).
		std::atomic<uint64_t> m_currentFrameMono{0};
		std::atomic<bool>     m_drainFlag{false};

		// Next PTS the encoder is expecting. Worker only consumes a recording
		// slot whose videoPts matches this — guarantees in-order delivery to
		// libavcodec even though the staging-buffer ring rotates and slot index
		// no longer correlates with PTS after the first wrap. Reset to 0 when
		// recording starts.
		std::atomic<int64_t>  m_nextExpectedPts{0};

		// Pending one-shot screenshot. Cleared once OnRenderRecord schedules a slot.
		std::atomic<bool>  m_screenshotPending{false};

		// Recording state — writes happen on the render thread (toggle, OnRenderRecord)
		// and worker thread (encoder I/O). Protected by m_recordingMutex; the bool
		// is exposed atomically for cheap UI polling.
		std::atomic<bool>  m_recording{false};
		std::atomic<uint32_t> m_framesCaptured{0};
		std::atomic<uint32_t> m_framesDropped{0};
		std::atomic<int64_t>  m_logicalNanos{0};      // updated when a recording frame is queued
		std::chrono::steady_clock::time_point m_recordingStartWall{};
		std::filesystem::path m_recordingPath;
		mutable std::mutex m_recordingMutex;
		std::unique_ptr<VideoEncoder>  m_videoEncoder;
		std::unique_ptr<StillEncoder>  m_stillEncoder;
		bool m_videoEncoderOpened = false;
		VkExtent2D m_recordingExtent{0, 0};            // locked at first frame; resize ends recording
		VkFormat   m_recordingFormat = VK_FORMAT_UNDEFINED;

		// Last paths surfaced to the UI footer.
		mutable std::mutex m_pathMutex;
		std::string        m_lastScreenshotPath;
		std::string        m_lastRecordingPath;

		std::function<void(std::string)> m_onScreenshotSaved;
		std::function<void(std::string)> m_onRecordingSaved;

		// Wall-clock origin so GetLogicalTimeSeconds returns a sane "time since
		// startup" when not recording in FixedStep mode.
		std::chrono::steady_clock::time_point m_processStart{};
	};

} // namespace Capture
