#include "PerformancePanel.h"
#include "UIStyle.h"
#include <algorithm>
#include <cstdio>

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

	// Single-line stats
	ImGui::TextColored(UIStyle::kTextDim, "FPS");
	ImGui::SameLine(0, 3);
	ImGui::Text("%.0f", m_fps);
	ImGui::SameLine(0, 10);
	ImGui::TextColored(UIStyle::kTextDim, "Frame");
	ImGui::SameLine(0, 3);
	ImGui::Text("%.1fms", m_frame_ms);
	ImGui::SameLine(0, 10);
	ImGui::TextColored(UIStyle::kTextDim, "GPU");
	ImGui::SameLine(0, 3);
	ImGui::Text("%.1fms", m_gpu_ms);

	// Frame time plot
	float maxFrame = *std::max_element(m_frame_times.begin(), m_frame_times.end());
	if (maxFrame < 1.0f) maxFrame = 1.0f;

	char overlay[32];
	snprintf(overlay, sizeof(overlay), "%.1f ms", m_frame_ms);
	ImGui::PlotLines("##frametime", m_frame_times.data(), HISTORY_SIZE,
		(int)m_offset, overlay, 0.0f, maxFrame * 1.5f, ImVec2(-1, 40));

	// GPU time plot
	float maxGPU = *std::max_element(m_gpu_times.begin(), m_gpu_times.end());
	if (maxGPU < 1.0f) maxGPU = 1.0f;

	snprintf(overlay, sizeof(overlay), "%.1f ms", m_gpu_ms);
	ImGui::PlotLines("##gputime", m_gpu_times.data(), HISTORY_SIZE,
		(int)m_offset, overlay, 0.0f, maxGPU * 1.5f, ImVec2(-1, 40));

	ImGui::End();
}
