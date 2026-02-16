#pragma once

#include "imgui.h"
#include "ImGuiLogSink.h"
#include <memory>

class OutputPanel {
private:
	std::shared_ptr<ImGuiLogSink> m_sink;
	bool m_auto_scroll = true;
	bool m_show_trace = false;
	bool m_show_debug = true;
	bool m_show_info = true;
	bool m_show_warn = true;
	bool m_show_error = true;

	ImVec4 GetColorForLevel(spdlog::level::level_enum level) const;
	bool ShouldShow(spdlog::level::level_enum level) const;

public:
	void SetSink(std::shared_ptr<ImGuiLogSink> sink) { m_sink = sink; }
	void Draw();
};
