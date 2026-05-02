#pragma once

#include "imgui.h"
#include "RenderGraphTypes.h"
#include "GPUProfiler.h"

class RenderGraphPanel {
public:
	enum class Mode { Graph, Table };

	void SetSnapshot(const GraphSnapshot* snapshot) { m_snapshot = snapshot; m_selectedPass = -1; }
	void SetMetrics(const GPUProfiler::PerformanceMetrics* metrics) { m_metrics = metrics; }
	void Draw();

private:
	const GraphSnapshot* m_snapshot = nullptr;
	const GPUProfiler::PerformanceMetrics* m_metrics = nullptr;
	int  m_selectedPass = -1;
	Mode m_mode = Mode::Graph;

	// Pan/zoom for the DAG canvas. Persistent across frames so the user can
	// dolly-tour large graphs.
	ImVec2 m_pan{0, 0};
	float  m_zoom = 1.0f;

	void DrawCanvas();
	void DrawTable();
	void DrawSelectedDetail();
};
