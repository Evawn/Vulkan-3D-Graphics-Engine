#pragma once

#include "imgui.h"
#include "RenderGraphTypes.h"
#include "GPUProfiler.h"

class RenderGraphPanel {
private:
	const GraphSnapshot* m_snapshot = nullptr;
	const GPUProfiler::PerformanceMetrics* m_metrics = nullptr;
	int m_selectedPass = -1;

public:
	void SetSnapshot(const GraphSnapshot* snapshot) { m_snapshot = snapshot; m_selectedPass = -1; }
	void SetMetrics(const GPUProfiler::PerformanceMetrics* metrics) { m_metrics = metrics; }
	void Draw();
};
