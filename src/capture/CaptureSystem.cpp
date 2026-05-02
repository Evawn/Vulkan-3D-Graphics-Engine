#include "CaptureSystem.h"
#include "StillEncoder.h"
#include "VideoEncoder.h"

#include "vk_mem_alloc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace Capture {

	namespace {
		// Ring slot count = framesInFlight + headroom. With 5 slots at 60fps the
		// worker has ~50ms to drain a slot before the same frame index returns
		// and the slot would be needed again. Bigger ring = more memory headroom
		// (~8 MB per slot at 1080p × RGBA), smaller = tighter encoder budget.
		constexpr uint32_t kRingHeadroom = 3;

		bool FormatIsBgra(VkFormat fmt) {
			return fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM;
		}

		std::string TimestampedFilename(const char* prefix, const char* ext) {
			using clock = std::chrono::system_clock;
			std::time_t t = clock::to_time_t(clock::now());
			std::tm tm{};
		#ifdef _WIN32
			localtime_s(&tm, &t);
		#else
			localtime_r(&t, &tm);
		#endif
			std::ostringstream oss;
			oss << prefix << std::put_time(&tm, "%Y%m%d_%H%M%S") << ext;
			return oss.str();
		}

	}

	// =============================================================================
	// Lifecycle
	// =============================================================================

	CaptureSystem::CaptureSystem() = default;

	CaptureSystem::~CaptureSystem() {
		Shutdown();
	}

	void CaptureSystem::Init(const CaptureSystemConfig& cfg) {
		m_cfg = cfg;
		m_processStart = std::chrono::steady_clock::now();
		m_stillEncoder = std::make_unique<StillEncoder>();

		const size_t ringSize = static_cast<size_t>(m_cfg.maxFramesInFlight + kRingHeadroom);
		m_ringSize = ringSize;
		m_ring     = std::make_unique<Slot[]>(ringSize);
		m_currentFrameMono.store(0, std::memory_order_release);

		m_workerExit.store(false, std::memory_order_release);
		m_worker = std::thread(&CaptureSystem::WorkerLoop, this);

		spdlog::get("App")->debug("CaptureSystem: initialized ({} ring slots)", ringSize);
	}

	void CaptureSystem::Shutdown() {
		// Tear down recording first so the encoder gets a clean Close (with
		// drain). DrainRing waits on every GpuPending slot via vkGetEventStatus
		// — must run before joining the worker.
		if (m_recording.load(std::memory_order_acquire)) {
			m_recording.store(false, std::memory_order_release);
		}
		DrainRing();
		CloseVideoEncoder();

		// Stop the worker. Notify under the mutex per the canonical CV idiom.
		{
			std::lock_guard<std::mutex> lk(m_slotMutex);
			m_workerExit.store(true, std::memory_order_release);
		}
		m_workerCv.notify_all();
		if (m_worker.joinable()) m_worker.join();

		// Staging buffers are shared_ptrs — they release on m_ring.reset(). No
		// per-slot Vulkan handles to free explicitly any more (the previous
		// VkEvent design was replaced with a frame-counter scheme for MoltenVK
		// portability).
		m_ring.reset();
		m_ringSize = 0;
		m_stillEncoder.reset();
	}

	// =============================================================================
	// Public requests
	// =============================================================================

	void CaptureSystem::RequestScreenshot() {
		m_screenshotPending.store(true, std::memory_order_release);
	}

	void CaptureSystem::ToggleRecording() {
		const bool wasActive = m_recording.load(std::memory_order_acquire);
		if (wasActive) {
			// Stop: flip state first so OnRenderRecord stops queueing new frames,
			// then drain the ring and close the encoder. Path stays in
			// m_lastRecordingPath for the UI footer.
			m_recording.store(false, std::memory_order_release);
			DrainRing();
			CloseVideoEncoder();
			std::string finished;
			{
				std::lock_guard<std::mutex> lk(m_pathMutex);
				finished = m_lastRecordingPath = m_recordingPath.string();
			}
			spdlog::get("App")->info("Recording stopped: {} ({} frames, {} dropped)",
			                         finished,
			                         m_framesCaptured.load(),
			                         m_framesDropped.load());
			if (m_onRecordingSaved && !finished.empty()) m_onRecordingSaved(finished);
		} else {
			// Start: pick output path now (the worker will need it on first frame
			// to open the encoder lazily). Counters reset; encoder opens on first
			// OnRenderRecord call when we know the extent.
			m_framesCaptured.store(0, std::memory_order_release);
			m_framesDropped.store(0, std::memory_order_release);
			m_nextExpectedPts.store(0, std::memory_order_release);
			m_recordingStartWall = std::chrono::steady_clock::now();
			// Logical clock anchors at wall-clock-at-record-start so FixedStep
			// produces a continuous time signal instead of jumping back to zero
			// when recording starts. Techniques that read GetLogicalTimeSeconds()
			// won't see their animations rewind on toggle.
			const auto initial = std::chrono::duration_cast<std::chrono::nanoseconds>(
				m_recordingStartWall - m_processStart).count();
			m_logicalNanos.store(initial, std::memory_order_release);
			m_recordingExtent = {0, 0};
			m_recordingFormat = VK_FORMAT_UNDEFINED;
			m_videoEncoderOpened = false;

			const std::string filename = TimestampedFilename("recording_", ".mp4");
			m_recordingPath = m_cfg.recordingsDir / filename;
			m_recording.store(true, std::memory_order_release);
			spdlog::get("App")->info("Recording started: {}", m_recordingPath.string());
		}
	}

	// =============================================================================
	// Slot acquisition (ring management)
	// =============================================================================

	CaptureSystem::Slot* CaptureSystem::AcquireSlot(VkExtent2D extent, VkFormat format,
	                                               bool isRecordingFrame,
	                                               BackPressure backPressure) {
		const VkDeviceSize required = static_cast<VkDeviceSize>(extent.width) *
		                              extent.height * 4;
		auto findFree = [&]() -> Slot* {
			for (size_t i = 0; i < m_ringSize; ++i) {
				if (m_ring[i].state.load(std::memory_order_acquire) == SlotState::Free) {
					return &m_ring[i];
				}
			}
			return nullptr;
		};

		std::unique_lock<std::mutex> lk(m_slotMutex);

		Slot* slot = findFree();
		if (!slot && backPressure == BackPressure::Block) {
			// Block until any slot is free. The worker thread transitions slots
			// from WorkerOwned → Free and notifies the CV. We re-scan after each
			// wake-up because a different slot may free first.
			m_workerCv.wait(lk, [&]{
				slot = findFree();
				return slot != nullptr;
			});
		}
		if (!slot) {
			// Drop policy: ring full, nothing free. Caller bumps the dropped counter.
			return nullptr;
		}

		// Slot reservation. The next state change happens device-side
		// (vkCmdSetEvent in the recorded copy commands) and then on the worker
		// thread when it observes the event.
		slot->state.store(SlotState::GpuPending, std::memory_order_release);
		slot->extent      = extent;
		slot->format      = format;
		slot->swizzleBgra = FormatIsBgra(format);
		slot->kind        = isRecordingFrame ? JobKind::RecordingFrame : JobKind::Screenshot;

		// Staging buffer: only realloc when current capacity isn't enough (we
		// never shrink). HOST_VISIBLE via VWrap::CreateStaging — VMA picks
		// system RAM on integrated GPUs and a host-visible heap on discrete.
		if (!slot->staging || slot->stagingSize < required) {
			slot->staging.reset();
			slot->staging = VWrap::Buffer::CreateStaging(m_cfg.allocator, required);
			slot->stagingSize = required;
		}
		return slot;
	}

	// =============================================================================
	// Per-frame integration (render thread)
	// =============================================================================

	void CaptureSystem::OnRenderRecord(
		std::shared_ptr<VWrap::CommandBuffer> cmd,
		uint32_t                              /*frameIndex*/,
		std::shared_ptr<VWrap::Image>         image,
		VkImageLayout                         currentLayout,
		VkFormat                              format,
		VkExtent2D                            extent)
	{
		if (extent.width == 0 || extent.height == 0) return;

		const bool wantScreenshot = m_screenshotPending.exchange(false, std::memory_order_acq_rel);
		const bool recording      = m_recording.load(std::memory_order_acquire);

		// Hard cap auto-stop. Checked here (render thread, before queueing new
		// frames) so the boundary frame becomes part of the file. Uses *logical*
		// time in FixedStep so the cap counts simulated seconds, not wall-clock
		// (a slow render shouldn't shorten the resulting clip).
		if (recording) {
			const auto wall = std::chrono::steady_clock::now() - m_recordingStartWall;
			const double wallSec = std::chrono::duration<double>(wall).count();
			const double effective = (m_options.pacing == Pacing::FixedStep)
				? static_cast<double>(m_framesCaptured.load()) / std::max(1, m_options.targetFps)
				: wallSec;
			if (effective >= static_cast<double>(m_options.maxDurationSeconds)) {
				spdlog::get("App")->info("Recording hit hard cap of {}s — auto-stopping",
				                          m_options.maxDurationSeconds);
				ToggleRecording();
				return;
			}
		}

		// Recording locks extent on first frame. If the user resizes mid-recording
		// the extent changes — we end the recording rather than try to scale.
		if (recording && m_recordingExtent.width != 0 &&
		    (m_recordingExtent.width != extent.width || m_recordingExtent.height != extent.height)) {
			spdlog::get("App")->warn("Recording extent changed ({}x{} -> {}x{}); auto-stopping",
				m_recordingExtent.width, m_recordingExtent.height,
				extent.width, extent.height);
			ToggleRecording();
			return;
		}

		const bool wantRecordingFrame = recording;
		if (!wantScreenshot && !wantRecordingFrame) return;

		// Lazy-open the encoder once we know the extent. Doing it here (render
		// thread) avoids any cross-thread Open/PushFrame ordering questions —
		// the worker only ever calls PushFrame on an already-open encoder.
		if (wantRecordingFrame && !m_videoEncoderOpened) {
			m_recordingExtent = extent;
			m_recordingFormat = format;
			OpenVideoEncoder(extent);
			m_videoEncoderOpened = true;
		}

		// Bump the monotonic frame counter exactly once per OnRenderRecord call.
		// Worker readiness is keyed off this; the bump must happen before slots
		// are tagged so each slot.recordedAtFrame reflects the current frame.
		const uint64_t thisFrame = m_currentFrameMono.fetch_add(1, std::memory_order_acq_rel) + 1;

		// Two captures in one frame (screenshot + recording) acquire two slots.
		// Both copy from the same source image; we issue separate copy commands
		// so each slot's staging gets its own bytes.
		auto recordCopyInto = [&](Slot* slot, JobKind kind) {
			if (!slot) return;
			(void)kind;

			cmd->CmdTransitionImageLayout(image, format,
				currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

			VkBufferImageCopy region{};
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel       = 0;
			region.imageSubresource.baseArrayLayer = 0;
			region.imageSubresource.layerCount     = 1;
			region.imageExtent = { extent.width, extent.height, 1 };

			vkCmdCopyImageToBuffer(cmd->Get(), image->Get(),
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				slot->staging->Get(), 1, &region);

			cmd->CmdTransitionImageLayout(image, format,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, currentLayout);

			slot->recordedAtFrame = thisFrame;
		};

		if (wantScreenshot) {
			Slot* slot = AcquireSlot(extent, format, /*isRecordingFrame*/false,
			                         BackPressure::Drop /* screenshots never block */);
			if (slot) {
				recordCopyInto(slot, JobKind::Screenshot);
			} else {
				spdlog::get("App")->warn("Screenshot dropped: no free slot");
			}
		}

		if (wantRecordingFrame) {
			const int64_t pts = static_cast<int64_t>(m_framesCaptured.load(std::memory_order_acquire));
			Slot* slot = AcquireSlot(extent, format, /*isRecordingFrame*/true,
			                         m_options.backPressure);
			if (slot) {
				slot->videoPts = pts;
				recordCopyInto(slot, JobKind::RecordingFrame);
				m_framesCaptured.fetch_add(1, std::memory_order_release);

				// Logical-time advance: FixedStep is exactly 1/fps per frame,
				// RealTime mirrors wall-clock so techniques behave identically.
				const int64_t nanosPerFrame = static_cast<int64_t>(1'000'000'000LL /
					std::max(1, m_options.targetFps));
				if (m_options.pacing == Pacing::FixedStep) {
					m_logicalNanos.fetch_add(nanosPerFrame, std::memory_order_release);
				} else {
					const auto wall = std::chrono::steady_clock::now() - m_recordingStartWall;
					m_logicalNanos.store(
						std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count(),
						std::memory_order_release);
				}
			} else {
				m_framesDropped.fetch_add(1, std::memory_order_release);
			}
		}

		// Wake the worker — it polls slot events but blocks on the CV between
		// passes to keep idle CPU low.
		m_workerCv.notify_one();
	}

	// =============================================================================
	// Worker thread
	// =============================================================================

	void CaptureSystem::WorkerLoop() {
		using namespace std::chrono_literals;
		while (true) {
			// Sleep on the CV until either someone notifies us OR shutdown. We
			// then sweep the ring for slots whose VkEvent has been signaled by
			// the device. Polling without sleeping would peg a core; CV-based
			// wake-up is the standard producer-consumer pattern.
			{
				std::unique_lock<std::mutex> lk(m_slotMutex);
				m_workerCv.wait_for(lk, 5ms, [&]{
					if (m_workerExit.load(std::memory_order_acquire)) return true;
					for (size_t i = 0; i < m_ringSize; ++i) {
						if (m_ring[i].state.load(std::memory_order_acquire) == SlotState::GpuPending) return true;
					}
					return false;
				});
			}
			if (m_workerExit.load(std::memory_order_acquire)) return;

			// Drain the ring in PTS order, not slot-index order. Once the slot
			// ring wraps (frame 6 reuses slot 0 etc.) slot indices no longer
			// correspond to PTS — so iterating naively pushes frames to the
			// encoder out of order, which trips libavcodec's monotonic-DTS
			// invariant and produces visible color flicker / macroblock
			// corruption on playback.
			//
			// Algorithm: keep finding the slot whose PTS matches m_nextExpectedPts
			// (recording frames) or any ready Screenshot slot, process it, repeat
			// until no further progress is possible this pass.
			while (true) {
				const bool drain = m_drainFlag.load(std::memory_order_acquire);
				const uint64_t now = m_currentFrameMono.load(std::memory_order_acquire);
				const int64_t  expected = m_nextExpectedPts.load(std::memory_order_acquire);
				Slot* match = nullptr;
				for (size_t i = 0; i < m_ringSize; ++i) {
					Slot& slot = m_ring[i];
					if (slot.state.load(std::memory_order_acquire) != SlotState::GpuPending) continue;
					if (!drain && (now < slot.recordedAtFrame + m_cfg.maxFramesInFlight)) continue;
					if (slot.kind == JobKind::RecordingFrame && slot.videoPts != expected) continue;
					match = &slot;
					break;
				}
				if (!match) break;

				match->state.store(SlotState::WorkerOwned, std::memory_order_release);
				ProcessSlot(*match);
				if (match->kind == JobKind::RecordingFrame) {
					m_nextExpectedPts.fetch_add(1, std::memory_order_acq_rel);
				}
				match->state.store(SlotState::Free, std::memory_order_release);
				m_workerCv.notify_all();   // unblock any AcquireSlot Block-mode waiter
			}
		}
	}

	void CaptureSystem::ProcessSlot(Slot& slot) {
		// Map staging, swizzle BGRA→RGBA in place if needed, dispatch.
		void* mapped = nullptr;
		vmaMapMemory(m_cfg.allocator->Get(), slot.staging->GetAllocation(), &mapped);
		uint8_t* pixels = static_cast<uint8_t*>(mapped);

		if (slot.swizzleBgra) {
			const uint32_t pixelCount = slot.extent.width * slot.extent.height;
			for (uint32_t i = 0; i < pixelCount; ++i) {
				std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
			}
		}

		switch (slot.kind) {
			case JobKind::Screenshot: {
				const std::string filename = TimestampedFilename("screenshot_", ".png");
				const std::filesystem::path p = m_cfg.screenshotsDir / filename;
				if (m_stillEncoder->Write(p, pixels, slot.extent.width, slot.extent.height)) {
					{
						std::lock_guard<std::mutex> lk(m_pathMutex);
						m_lastScreenshotPath = p.string();
					}
					if (m_onScreenshotSaved) m_onScreenshotSaved(p.string());
				}
				break;
			}
			case JobKind::RecordingFrame: {
				if (m_videoEncoder && m_videoEncoder->IsOpen()) {
					m_videoEncoder->PushFrame(pixels, slot.videoPts);
				}
				break;
			}
		}

		vmaUnmapMemory(m_cfg.allocator->Get(), slot.staging->GetAllocation());
	}

	// =============================================================================
	// Encoder lifecycle (render thread)
	// =============================================================================

	void CaptureSystem::OpenVideoEncoder(VkExtent2D extent) {
		std::lock_guard<std::mutex> lk(m_recordingMutex);
		m_videoEncoder = std::make_unique<VideoEncoder>();
		const bool ok = m_videoEncoder->Open(
			m_recordingPath,
			extent.width, extent.height,
			static_cast<uint32_t>(m_options.targetFps),
			static_cast<uint32_t>(m_options.bitrateKbps));
		if (!ok) {
			spdlog::get("App")->error("CaptureSystem: failed to open VideoEncoder; recording aborted");
			m_videoEncoder.reset();
			m_recording.store(false, std::memory_order_release);
		}
	}

	void CaptureSystem::CloseVideoEncoder() {
		std::lock_guard<std::mutex> lk(m_recordingMutex);
		if (m_videoEncoder) {
			m_videoEncoder->Close();
			m_videoEncoder.reset();
		}
		m_videoEncoderOpened = false;
	}

	void CaptureSystem::DrainRing() {
		// vkDeviceWaitIdle is the cleanest way to guarantee every queued slot's
		// GPU copy has completed without entangling with FrameController's per-
		// frame fences. The drain flag tells the worker thread to bypass its
		// frame-counter readiness check (which only advances when more frames
		// are submitted — and during a drain, no new frames are being queued).
		if (m_cfg.device) {
			vkDeviceWaitIdle(m_cfg.device->Get());
		}
		m_drainFlag.store(true, std::memory_order_release);
		m_workerCv.notify_all();

		using namespace std::chrono_literals;
		while (true) {
			bool anyBusy = false;
			for (size_t i = 0; i < m_ringSize; ++i) {
				const auto st = m_ring[i].state.load(std::memory_order_acquire);
				if (st != SlotState::Free) { anyBusy = true; break; }
			}
			if (!anyBusy) break;
			m_workerCv.notify_all();
			std::this_thread::sleep_for(2ms);
		}
		m_drainFlag.store(false, std::memory_order_release);
	}

	// =============================================================================
	// Status & introspection
	// =============================================================================

	RecordingStatus CaptureSystem::GetStatus() const {
		RecordingStatus s;
		s.active         = m_recording.load(std::memory_order_acquire);
		s.framesCaptured = m_framesCaptured.load(std::memory_order_acquire);
		s.framesDropped  = m_framesDropped.load(std::memory_order_acquire);
		if (s.active) {
			if (m_options.pacing == Pacing::FixedStep) {
				s.elapsedSeconds = static_cast<float>(s.framesCaptured) /
				                   std::max(1, m_options.targetFps);
			} else {
				const auto wall = std::chrono::steady_clock::now() - m_recordingStartWall;
				s.elapsedSeconds = std::chrono::duration<float>(wall).count();
			}
		}
		{
			std::lock_guard<std::mutex> lk(m_pathMutex);
			s.outputPath = m_lastRecordingPath.empty() ? m_recordingPath.string() : m_lastRecordingPath;
		}
		return s;
	}

	std::string CaptureSystem::GetLastScreenshotPath() const {
		std::lock_guard<std::mutex> lk(m_pathMutex);
		return m_lastScreenshotPath;
	}

	std::string CaptureSystem::GetLastRecordingPath() const {
		std::lock_guard<std::mutex> lk(m_pathMutex);
		return m_lastRecordingPath;
	}

	double CaptureSystem::GetLogicalTimeSeconds() const {
		// FixedStep + recording: logical clock = (wall-clock at record start)
		// + frames * (1/fps). Continuous with wall-clock at the moment of
		// toggle, then advances deterministically per captured frame. Other
		// modes return wall-clock-since-process-start, which matches the
		// pre-refactor technique behavior so animations are unchanged when no
		// recording is active.
		if (m_recording.load(std::memory_order_acquire) &&
		    m_options.pacing == Pacing::FixedStep) {
			return static_cast<double>(m_logicalNanos.load(std::memory_order_acquire)) / 1e9;
		}
		const auto wall = std::chrono::steady_clock::now() - m_processStart;
		return std::chrono::duration<double>(wall).count();
	}

} // namespace Capture
