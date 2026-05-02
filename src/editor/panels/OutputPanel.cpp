#include "OutputPanel.h"
#include "UIStyle.h"

#include <algorithm>
#include <cstring>

bool OutputPanel::ShouldShow(spdlog::level::level_enum lvl) const {
	switch (lvl) {
		case spdlog::level::trace:    return m_show_trace;
		case spdlog::level::debug:    return m_show_debug;
		case spdlog::level::info:     return m_show_info;
		case spdlog::level::warn:     return m_show_warn;
		case spdlog::level::err:
		case spdlog::level::critical: return m_show_error;
		default: return true;
	}
}

void OutputPanel::RefreshSources() {
	if (!m_sink) return;
	uint64_t v = m_sink->GetVersion();
	if (v == m_last_sink_version) return;
	m_last_sink_version = v;

	std::unordered_set<std::string> seen;
	for (const auto& e : m_sink->GetEntries()) seen.insert(e.logger_name);

	std::vector<std::string> sorted(seen.begin(), seen.end());
	std::sort(sorted.begin(), sorted.end());
	m_known_sources = std::move(sorted);
}

void OutputPanel::Draw() {
	RefreshSources();

	const size_t err_count  = m_sink ? m_sink->GetErrorCount() : 0;
	const size_t warn_count = m_sink ? m_sink->GetWarnCount()  : 0;

	// Title-bar badges. ImGui doesn't render formatting in window titles, so we
	// embed the counts in the window name. The dock id is "Console" — the
	// trailing "###Console" anchors the imgui.ini layout to the same window
	// even when the title text changes.
	char title[96];
	if (err_count > 0)
		snprintf(title, sizeof(title), "Console  [%zuE %zuW]###Console", err_count, warn_count);
	else if (warn_count > 0)
		snprintf(title, sizeof(title), "Console  [%zuW]###Console", warn_count);
	else
		snprintf(title, sizeof(title), "Console###Console");

	ImGui::Begin(title);

	// === Banner: latest unread error ===
	if (m_pause_on_error && err_count > m_banner_dismissed_err) {
		// Find the most recent error entry.
		const LogEntry* recent = nullptr;
		if (m_sink) {
			const auto& entries = m_sink->GetEntries();
			for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
				if (it->level == spdlog::level::err || it->level == spdlog::level::critical) {
					recent = &*it;
					break;
				}
			}
		}
		if (recent) {
			ImGui::PushStyleColor(ImGuiCol_ChildBg, UIStyle::Alpha(UIStyle::kBudgetOver, 0.12f));
			ImGui::BeginChild("##banner", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 1.6f), true);
			ImGui::TextColored(UIStyle::kBudgetOver, "ERROR");
			ImGui::SameLine(0, 8);
			ImGui::TextColored(UIStyle::kTextDim, "[%s]", recent->logger_name.c_str());
			ImGui::SameLine(0, 6);
			ImGui::TextUnformatted(recent->message.c_str());
			ImGui::SameLine();
			float w = ImGui::GetContentRegionAvail().x;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 60);
			if (ImGui::SmallButton("Dismiss")) m_banner_dismissed_err = err_count;
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}
	}

	// === Toolbar row ===
	auto LevelBtn = [](const char* lbl, bool* on, ImVec4 col) {
		ImVec4 bg = *on ? UIStyle::Alpha(col, 0.25f) : UIStyle::kBgLight;
		ImVec4 fg = *on ? col : UIStyle::kTextDim;
		ImGui::PushStyleColor(ImGuiCol_Button, bg);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle::Alpha(col, 0.4f));
		ImGui::PushStyleColor(ImGuiCol_Text, fg);
		if (ImGui::SmallButton(lbl)) *on = !*on;
		ImGui::PopStyleColor(3);
	};

	LevelBtn("T", &m_show_trace, UIStyle::GetLogLevelColor(spdlog::level::trace)); ImGui::SameLine(0, 2);
	LevelBtn("D", &m_show_debug, UIStyle::GetLogLevelColor(spdlog::level::debug)); ImGui::SameLine(0, 2);
	LevelBtn("I", &m_show_info,  UIStyle::GetLogLevelColor(spdlog::level::info));  ImGui::SameLine(0, 2);
	LevelBtn("W", &m_show_warn,  UIStyle::GetLogLevelColor(spdlog::level::warn));  ImGui::SameLine(0, 2);
	LevelBtn("E", &m_show_error, UIStyle::GetLogLevelColor(spdlog::level::err));

	ImGui::SameLine(0, 8);
	// Source filter dropdown
	if (ImGui::SmallButton("sources")) ImGui::OpenPopup("##srcs");
	if (ImGui::BeginPopup("##srcs")) {
		for (const auto& s : m_known_sources) {
			bool enabled = m_disabled_sources.count(s) == 0;
			if (ImGui::Checkbox(s.c_str(), &enabled)) {
				if (enabled) m_disabled_sources.erase(s);
				else         m_disabled_sources.insert(s);
			}
		}
		ImGui::EndPopup();
	}

	ImGui::SameLine(0, 8);
	ImGui::SetNextItemWidth(160);
	char buf[256];
	std::strncpy(buf, m_search.c_str(), sizeof(buf));
	buf[sizeof(buf) - 1] = '\0';
	if (ImGui::InputTextWithHint("##search", "search...", buf, sizeof(buf))) {
		m_search = buf;
	}

	ImGui::SameLine(0, 8);
	ImGui::Checkbox("auto", &m_auto_scroll);
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("dedup", &m_dedup);
	ImGui::SameLine(0, 6);
	ImGui::Checkbox("pause on error", &m_pause_on_error);

	ImGui::SameLine();
	float w = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, w - 50));
	if (ImGui::SmallButton("Clear") && m_sink) {
		m_sink->Clear();
		m_banner_dismissed_err = 0;
	}

	// === Body ===
	ImGui::BeginChild("##log", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

	if (m_sink) {
		const auto& entries = m_sink->GetEntries();
		const std::string& s = m_search;

		// Walk entries newest-last. Dedup runs in a single pass with a tiny
		// state machine: track the last-rendered line; if the next visible
		// line matches it textually + level, increment a count instead.
		const LogEntry* last = nullptr;
		uint32_t        run  = 0;
		auto flush_last = [&]() {
			if (!last) return;
			ImVec4 col = UIStyle::GetLogLevelColor(last->level);
			if (m_show_source) {
				ImGui::TextColored(UIStyle::kTextDim, "[%s]", last->logger_name.c_str());
				ImGui::SameLine(0, 4);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, col);
			if (run > 1) {
				ImGui::Text("[x%u] %s", run, last->message.c_str());
			} else {
				ImGui::TextUnformatted(last->message.c_str());
			}
			ImGui::PopStyleColor();
		};

		for (const auto& e : entries) {
			if (!ShouldShow(e.level)) continue;
			if (m_disabled_sources.count(e.logger_name)) continue;
			if (!s.empty() && e.message.find(s) == std::string::npos &&
			    e.logger_name.find(s) == std::string::npos) continue;

			if (m_dedup && last &&
			    last->level == e.level &&
			    last->logger_name == e.logger_name &&
			    last->message == e.message) {
				run++;
				continue;
			}
			flush_last();
			last = &e;
			run = 1;
		}
		flush_last();
	}

	if (m_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
		ImGui::SetScrollHereY(1.0f);
	}

	ImGui::EndChild();
	ImGui::End();
}
