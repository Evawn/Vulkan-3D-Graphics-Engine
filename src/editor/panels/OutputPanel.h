#pragma once

#include "imgui.h"
#include "ImGuiLogSink.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <cstdint>

// In-app console: search, level/source filtering, dedup, error/warn counters,
// pause-on-error sticky banner. Reads from ImGuiLogSink, which fans out the
// project's spdlog loggers ("App" / "Render" / "VWrap" / "Input" / "GPU").
//
// File name kept as "OutputPanel.h" to avoid disrupting the CMake glob during
// the editor overhaul; the dock window title is "Console".
class OutputPanel {
public:
	void SetSink(std::shared_ptr<ImGuiLogSink> sink) { m_sink = std::move(sink); }

	// Reset cumulative warn/error counters AND the dismissed-banner state.
	void ResetBanner() { m_banner_dismissed_err = 0; }

	// Public for status-bar / menu-badge consumers.
	size_t GetWarnCount()  const { return m_sink ? m_sink->GetWarnCount()  : 0; }
	size_t GetErrorCount() const { return m_sink ? m_sink->GetErrorCount() : 0; }

	void Draw();

private:
	std::shared_ptr<ImGuiLogSink> m_sink;

	// Filters
	bool m_show_trace = false;
	bool m_show_debug = false;
	bool m_show_info  = true;
	bool m_show_warn  = true;
	bool m_show_error = true;
	std::string m_search;            // substring filter
	std::unordered_set<std::string> m_disabled_sources; // logger names with their box unchecked

	// Behavior
	bool m_auto_scroll = true;
	bool m_pause_on_error = false;
	bool m_show_time = false;        // if a timestamp prefix is desired (no-op until source-loc plumbed)
	bool m_show_source = true;
	bool m_dedup = true;             // collapse consecutive identical messages

	// Banner state — "first error since dismissed". Compared against
	// sink->GetErrorCount() to detect a NEW error after dismissal.
	size_t m_banner_dismissed_err = 0;

	// Cached source-name set (for the source filter dropdown).
	std::vector<std::string> m_known_sources;
	uint64_t m_last_sink_version = 0;

	void RefreshSources();
	bool ShouldShow(spdlog::level::level_enum lvl) const;
};
