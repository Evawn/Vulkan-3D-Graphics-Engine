#pragma once

#include "imgui.h"
#include "GPUProfiler.h"
#include "RenderGraphTypes.h"

#include <array>
#include <deque>
#include <string>
#include <unordered_map>

// Performance panel — frame timing, per-pass GPU timings, and the budget
// breakdown. Owns its own per-pass history (keyed by pass name) so pass
// reordering on graph rebuild doesn't lose history.
class PerformancePanel {
public:
	static constexpr size_t HISTORY_SIZE = 240; // 4 seconds @ 60Hz

	void Update(float fps, float gpuTimeMs, float frameTimeMs);
	void SetGraphSnapshot(const GraphSnapshot* snap)        { m_snapshot = snap; }
	void SetMetrics(const GPUProfiler::PerformanceMetrics* m){ m_metrics  = m; }

	void Draw();

	// Same per-pass history exposed for the viewport HUD / status bar.
	float GetGpuMs() const { return m_gpu_ms; }
	float GetFrameMs() const { return m_frame_ms; }
	float GetFps() const { return m_fps; }

private:
	// Per-pass ring buffer — keyed by pass name so graph rebuilds don't reset it.
	struct PassHistory {
		std::array<float, HISTORY_SIZE> samples{};
		size_t   offset = 0;
		size_t   filled = 0;  // how many samples accumulated; clamps at HISTORY_SIZE
		uint32_t color  = 0;  // packed RGBA, assigned at first sight

		void Push(float v);
		float Avg(size_t window = 60) const;
		float Max(size_t window = 60) const;
	};

	// Frame totals
	std::array<float, HISTORY_SIZE> m_frame_times{};
	std::array<float, HISTORY_SIZE> m_gpu_times{};
	size_t m_offset = 0;

	float m_fps = 0.0f;
	float m_gpu_ms = 0.0f;
	float m_frame_ms = 0.0f;

	std::unordered_map<std::string, PassHistory> m_pass_history;

	const GraphSnapshot* m_snapshot = nullptr;
	const GPUProfiler::PerformanceMetrics* m_metrics = nullptr;

	// Soft-max state for chart y-axes — decays slowly toward measured peak so
	// the axis doesn't rescale on every sample. Floored at 16.6ms (60Hz target)
	// so an idle scene still renders charts at a sensible scale.
	float m_axis_budget = 33.3f;   // budget strip
	float m_axis_history = 33.3f;  // stacked area chart

	void DrawBudgetStrip(float height);
	void DrawStackedHistory(float height);
	void DrawPassTable();

	uint32_t ColorForIndex(size_t idx) const;
};
