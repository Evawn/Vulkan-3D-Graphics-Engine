#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

// Shared, header-only types for the capture subsystem.
//
// The capture pipeline produces two artifact flavors:
//   - One-shot screenshots (PNG)
//   - Continuous recordings (h264 in MP4)
//
// Both flavors flow through the same staging-buffer ring and the same worker
// thread; they differ only in what the encoder side does with the pixels.
namespace Capture {

	// Frame pacing governs how the recording's *logical time* advances.
	//
	// RealTime: logical time = wall-clock time since BeginRecording. The MP4's
	//   PTS values reflect when each frame was actually rendered. If the engine
	//   stutters, the resulting video reflects that stutter (or has dropped frames,
	//   depending on the BackPressure policy). What you see during recording is
	//   what gets written.
	//
	// FixedStep: logical time advances by exactly 1/targetFps per captured frame,
	//   regardless of wall-clock. The engine renders as fast (or slow) as it can,
	//   but every frame represents 1/fps of simulated time. The MP4 plays back
	//   smoothly at exactly targetFps. This is the offline-cinematic mode — used
	//   when the technique is too expensive to hit 60fps live but you still want
	//   a 60fps demo reel. Requires logical-time plumbing in techniques (Phase 7);
	//   without it, animation will appear to slow-mo since wall-clock still
	//   drives technique animation.
	enum class Pacing {
		RealTime,
		FixedStep,
	};

	// What to do when a capture is requested but the staging-buffer ring is full
	// (the encoder hasn't drained slots fast enough).
	//
	// Drop: skip this frame's capture, increment a counter. Render keeps its
	//   wall-clock pace; the recording loses a frame. Right default for RealTime.
	//
	// Block: stall the render thread until a slot frees up (vkGetEventStatus poll
	//   loop on the oldest pending slot). Render can hitch but no frame is lost.
	//   Required for FixedStep mode where every frame must land or the resulting
	//   video has gaps in logical time.
	//
	// These two are independently switchable from the UI so we can A/B them while
	// tuning encoder throughput. Default pairing is RealTime+Drop / FixedStep+Block.
	enum class BackPressure {
		Drop,
		Block,
	};

	struct RecordingOptions {
		Pacing       pacing       = Pacing::RealTime;
		BackPressure backPressure = BackPressure::Drop;
		int          targetFps    = 60;
		int          maxDurationSeconds = 60;   // hard cap; auto-stop at this duration
		int          bitrateKbps  = 8000;       // ~8 Mbps for 1080p60 looks clean
	};

	// Live recording status, polled by the UI for the indicator + footer text.
	struct RecordingStatus {
		bool        active         = false;
		uint32_t    framesCaptured = 0;
		uint32_t    framesDropped  = 0;
		float       elapsedSeconds = 0.0f;     // logical time for FixedStep, wall-clock for RealTime
		std::string outputPath;                // populated when recording is active or just finished
	};

} // namespace Capture
