#include "MetricsPanel.h"
#include "UIStyle.h"
#include <algorithm>
#include <cstdio>

void MetricsPanel::Update(float fps, float gpuTimeMs, float frameTimeMs) {
	m_fps = fps;
	m_gpu_ms = gpuTimeMs;
	m_frame_ms = frameTimeMs;

	m_frame_times[m_offset] = frameTimeMs;
	m_gpu_times[m_offset] = gpuTimeMs;
	m_offset = (m_offset + 1) % HISTORY_SIZE;
}

void MetricsPanel::SetRenderers(std::vector<std::unique_ptr<RenderTechnique>>* renderers, size_t* activeIndex) {
	m_renderers = renderers;
	m_active_index = activeIndex;
}

void MetricsPanel::SetAllocator(VmaAllocator allocator, uint32_t heapCount) {
	m_allocator = allocator;
	m_heap_count = heapCount;
	m_heap_info.resize(heapCount);
}

void MetricsPanel::Draw() {
	ImGui::Begin("Metrics");

	// === Performance ===
	if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
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

		float maxFrame = *std::max_element(m_frame_times.begin(), m_frame_times.end());
		if (maxFrame < 1.0f) maxFrame = 1.0f;

		char overlay[32];
		snprintf(overlay, sizeof(overlay), "%.1f ms", m_frame_ms);
		ImGui::PlotLines("##frametime", m_frame_times.data(), HISTORY_SIZE,
			(int)m_offset, overlay, 0.0f, maxFrame * 1.5f, ImVec2(-1, 40));

		float maxGPU = *std::max_element(m_gpu_times.begin(), m_gpu_times.end());
		if (maxGPU < 1.0f) maxGPU = 1.0f;

		snprintf(overlay, sizeof(overlay), "%.1f ms", m_gpu_ms);
		ImGui::PlotLines("##gputime", m_gpu_times.data(), HISTORY_SIZE,
			(int)m_offset, overlay, 0.0f, maxGPU * 1.5f, ImVec2(-1, 40));
	}

	// === Render Stats ===
	if (ImGui::CollapsingHeader("Render Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (m_renderers && m_active_index) {
			auto& renderer = (*m_renderers)[*m_active_index];
			FrameStats stats = renderer->GetFrameStats();

			ImGui::TextColored(UIStyle::kTextDim, "Draw Calls");
			ImGui::SameLine(0, 3);
			ImGui::Text("%u", stats.drawCalls);
			ImGui::SameLine(0, 10);
			ImGui::TextColored(UIStyle::kTextDim, "Verts");
			ImGui::SameLine(0, 3);
			ImGui::Text("%u", stats.vertices);
			ImGui::SameLine(0, 10);
			ImGui::TextColored(UIStyle::kTextDim, "Indices");
			ImGui::SameLine(0, 3);
			ImGui::Text("%u", stats.indices);

			bool wireframe = renderer->GetWireframe();
			if (ImGui::Checkbox("Wireframe", &wireframe)) {
				renderer->SetWireframe(wireframe);
				if (m_wireframe_callback) m_wireframe_callback();
			}
		}
	}

	// === Memory ===
	if (ImGui::CollapsingHeader("Memory")) {
		if (m_allocator != VK_NULL_HANDLE) {
			// Throttle VMA queries to every 30 frames
			if (m_stats_throttle <= 0) {
				m_stats_throttle = 30;

				std::vector<VmaBudget> budgets(m_heap_count);
				vmaGetHeapBudgets(m_allocator, budgets.data());

				m_total_bytes = 0;
				m_allocation_count = 0;

				for (uint32_t i = 0; i < m_heap_count; i++) {
					m_heap_info[i].usage = budgets[i].usage;
					m_heap_info[i].budget = budgets[i].budget;
					m_total_bytes += budgets[i].usage;
				}

				VmaTotalStatistics stats{};
				vmaCalculateStatistics(m_allocator, &stats);
				m_allocation_count = stats.total.statistics.allocationCount;
			}
			m_stats_throttle--;

			for (uint32_t i = 0; i < m_heap_count; i++) {
				float usageMB = (float)m_heap_info[i].usage / (1024.0f * 1024.0f);
				float budgetMB = (float)m_heap_info[i].budget / (1024.0f * 1024.0f);
				float fraction = budgetMB > 0 ? usageMB / budgetMB : 0.0f;

				char label[64];
				snprintf(label, sizeof(label), "%.1f / %.0f MB", usageMB, budgetMB);
				ImGui::Text("Heap %u", i);
				ImGui::SameLine();
				ImGui::ProgressBar(fraction, ImVec2(-1, 0), label);
			}

			float totalMB = (float)m_total_bytes / (1024.0f * 1024.0f);
			ImGui::TextColored(UIStyle::kTextDim, "Allocations: %u  Total: %.1f MB", m_allocation_count, totalMB);
		} else {
			ImGui::TextColored(UIStyle::kTextDim, "No allocator available");
		}
	}

	ImGui::End();
}
