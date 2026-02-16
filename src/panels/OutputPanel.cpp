#include "OutputPanel.h"

ImVec4 OutputPanel::GetColorForLevel(spdlog::level::level_enum level) const {
	switch (level) {
	case spdlog::level::trace:    return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
	case spdlog::level::debug:    return ImVec4(0.4f, 0.7f, 1.0f, 1.0f);
	case spdlog::level::info:     return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
	case spdlog::level::warn:     return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
	case spdlog::level::err:      return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
	case spdlog::level::critical: return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
	default:                      return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	}
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

	// Filter checkboxes
	ImGui::Checkbox("Trace", &m_show_trace);
	ImGui::SameLine();
	ImGui::Checkbox("Debug", &m_show_debug);
	ImGui::SameLine();
	ImGui::Checkbox("Info", &m_show_info);
	ImGui::SameLine();
	ImGui::Checkbox("Warn", &m_show_warn);
	ImGui::SameLine();
	ImGui::Checkbox("Error", &m_show_error);
	ImGui::SameLine();

	if (ImGui::Button("Clear") && m_sink) {
		m_sink->Clear();
	}
	ImGui::SameLine();
	ImGui::Checkbox("Auto-scroll", &m_auto_scroll);

	ImGui::Separator();

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
