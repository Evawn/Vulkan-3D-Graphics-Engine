#include "PerformancePanel.h"
#include <algorithm>

void PerformancePanel::Update(float fps, float gpuTimeMs, float frameTimeMs) {
	m_fps = fps;
	m_gpu_ms = gpuTimeMs;
	m_frame_ms = frameTimeMs;

	m_frame_times[m_offset] = frameTimeMs;
	m_gpu_times[m_offset] = gpuTimeMs;
	m_offset = (m_offset + 1) % HISTORY_SIZE;
}

void PerformancePanel::Draw() {
	ImGui::Begin("Performance");

	ImGui::Text("FPS: %.1f", m_fps);
	ImGui::Text("Frame: %.2f ms", m_frame_ms);
	ImGui::Text("GPU: %.2f ms", m_gpu_ms);

	ImGui::Separator();

	float maxFrame = *std::max_element(m_frame_times.begin(), m_frame_times.end());
	if (maxFrame < 1.0f) maxFrame = 1.0f;

	ImGui::Text("Frame Time");
	ImGui::PlotLines("##frametime", m_frame_times.data(), HISTORY_SIZE,
		(int)m_offset, nullptr, 0.0f, maxFrame * 1.5f, ImVec2(0, 60));

	float maxGPU = *std::max_element(m_gpu_times.begin(), m_gpu_times.end());
	if (maxGPU < 1.0f) maxGPU = 1.0f;

	ImGui::Text("GPU Time");
	ImGui::PlotLines("##gputime", m_gpu_times.data(), HISTORY_SIZE,
		(int)m_offset, nullptr, 0.0f, maxGPU * 1.5f, ImVec2(0, 60));

	ImGui::End();
}
