#pragma once

#include "imgui.h"
#include <array>

class PerformancePanel {
private:
	static constexpr size_t HISTORY_SIZE = 120;
	std::array<float, HISTORY_SIZE> m_frame_times{};
	std::array<float, HISTORY_SIZE> m_gpu_times{};
	size_t m_offset = 0;

	float m_fps = 0.0f;
	float m_gpu_ms = 0.0f;
	float m_frame_ms = 0.0f;

public:
	void Update(float fps, float gpuTimeMs, float frameTimeMs);
	void Draw();
};
