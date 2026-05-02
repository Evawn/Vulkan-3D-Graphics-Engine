#pragma once

#include "imgui.h"
#include "vk_mem_alloc.h"
#include "RenderGraphTypes.h"

#include <array>
#include <vector>

// Memory panel — VMA heap residency with history, plus a per-resource bytes
// table derived from the live render-graph snapshot. Replaces the cohabiting
// "Memory" section that lived inside the old MetricsPanel.
class MemoryPanel {
public:
	static constexpr size_t HISTORY_SIZE = 240;

	void SetAllocator(VmaAllocator allocator, uint32_t heapCount);
	void SetGraphSnapshot(const GraphSnapshot* snap) { m_snapshot = snap; }

	// Called every frame. Internally throttles VMA queries to ~2Hz.
	void Update();

	void Draw();

	// Public for the status bar.
	uint64_t GetTotalBytes() const { return m_total_bytes; }
	uint32_t GetAllocationCount() const { return m_allocation_count; }

private:
	VmaAllocator m_allocator = VK_NULL_HANDLE;
	uint32_t m_heap_count = 0;
	int      m_stats_throttle = 0;

	struct HeapInfo {
		uint64_t usage = 0;
		uint64_t budget = 0;
		uint64_t high_water = 0;
		std::array<float, HISTORY_SIZE> history{}; // MB resident
		size_t   offset = 0;
	};
	std::vector<HeapInfo> m_heap_info;
	uint64_t m_total_bytes = 0;
	uint32_t m_allocation_count = 0;

	const GraphSnapshot* m_snapshot = nullptr;

	// Soft-max for the history chart's y-axis (MB). Decays slowly so the axis
	// doesn't rescale every time a new sample lands.
	float m_axis_history_mb = 64.0f;

	void DrawHeapStrip();
	void DrawHistoryChart(float height);
	void DrawResourceTable(float height);
};
