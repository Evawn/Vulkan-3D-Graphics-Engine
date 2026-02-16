#pragma once

#include "imgui.h"
#include "vk_mem_alloc.h"
#include "RenderTechnique.h"
#include <array>
#include <vector>
#include <memory>
#include <functional>

class MetricsPanel {
private:
	static constexpr size_t HISTORY_SIZE = 120;
	std::array<float, HISTORY_SIZE> m_frame_times{};
	std::array<float, HISTORY_SIZE> m_gpu_times{};
	size_t m_offset = 0;

	float m_fps = 0.0f;
	float m_gpu_ms = 0.0f;
	float m_frame_ms = 0.0f;

	// Render stats
	std::vector<std::unique_ptr<RenderTechnique>>* m_renderers = nullptr;
	size_t* m_active_index = nullptr;

	// Wireframe
	std::function<void()> m_wireframe_callback;

	// VMA
	VmaAllocator m_allocator = VK_NULL_HANDLE;
	uint32_t m_heap_count = 0;
	int m_stats_throttle = 0;

	// Cached VMA stats
	struct HeapInfo {
		VkDeviceSize usage = 0;
		VkDeviceSize budget = 0;
	};
	std::vector<HeapInfo> m_heap_info;
	uint32_t m_allocation_count = 0;
	VkDeviceSize m_total_bytes = 0;

public:
	void Update(float fps, float gpuTimeMs, float frameTimeMs);
	void SetRenderers(std::vector<std::unique_ptr<RenderTechnique>>* renderers, size_t* activeIndex);
	void SetWireframeCallback(std::function<void()> cb) { m_wireframe_callback = std::move(cb); }
	void SetAllocator(VmaAllocator allocator, uint32_t heapCount);
	void Draw();
};
