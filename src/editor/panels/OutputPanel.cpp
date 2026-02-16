#include "OutputPanel.h"
#include "UIStyle.h"

ImVec4 OutputPanel::GetColorForLevel(spdlog::level::level_enum level) const {
	return UIStyle::GetLogLevelColor(level);
}

bool OutputPanel::ShouldShow(spdlog::level::level_enum level) const {
	switch (level) {
	case spdlog::level::trace:    return m_show_trace;
	case spdlog::level::debug:    return m_show_debug;
	case spdlog::level::info:     return m_show_info;
	case spdlog::level::warn:     return m_show_warn;
	case spdlog::level::err:
	case spdlog::level::critical: return m_show_error;
	default:                      return true;
	}
}

void OutputPanel::Draw() {
	ImGui::Begin("Output");

	// Compact toggle button helper
	auto ToggleButton = [](const char* label, bool* value, ImVec4 activeColor) {
		if (*value) {
			ImGui::PushStyleColor(ImGuiCol_Button, UIStyle::Alpha(activeColor, 0.25f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle::Alpha(activeColor, 0.40f));
			ImGui::PushStyleColor(ImGuiCol_Text, activeColor);
		} else {
			ImGui::PushStyleColor(ImGuiCol_Button, UIStyle::kBgLight);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle::kBgLighter);
			ImGui::PushStyleColor(ImGuiCol_Text, UIStyle::kTextDim);
		}
		if (ImGui::SmallButton(label)) *value = !*value;
		ImGui::PopStyleColor(3);
	};

	// Filter toggles
	ToggleButton("T", &m_show_trace,  UIStyle::GetLogLevelColor(spdlog::level::trace));
	ImGui::SameLine(0, 2);
	ToggleButton("D", &m_show_debug,  UIStyle::GetLogLevelColor(spdlog::level::debug));
	ImGui::SameLine(0, 2);
	ToggleButton("I", &m_show_info,   UIStyle::GetLogLevelColor(spdlog::level::info));
	ImGui::SameLine(0, 2);
	ToggleButton("W", &m_show_warn,   UIStyle::GetLogLevelColor(spdlog::level::warn));
	ImGui::SameLine(0, 2);
	ToggleButton("E", &m_show_error,  UIStyle::GetLogLevelColor(spdlog::level::err));

	ImGui::SameLine(0, 8);
	if (ImGui::SmallButton("Clear") && m_sink) {
		m_sink->Clear();
	}

	ImGui::SameLine(0, 8);
	ImGui::Checkbox("##autoscroll", &m_auto_scroll);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-scroll");

	ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

	if (m_sink) {
		for (const auto& entry : m_sink->GetEntries()) {
			if (!ShouldShow(entry.level)) continue;

			ImVec4 color = GetColorForLevel(entry.level);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextUnformatted(("[" + entry.logger_name + "] " + entry.message).c_str());
			ImGui::PopStyleColor();
		}
	}

	if (m_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
		ImGui::SetScrollHereY(1.0f);
	}

	ImGui::EndChild();
	ImGui::End();
}
