#include "PerformancePanel.h"
#include "UIStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

void PerformancePanel::PassHistory::Push(float v) {
	samples[offset] = v;
	offset = (offset + 1) % PerformancePanel::HISTORY_SIZE;
	if (filled < PerformancePanel::HISTORY_SIZE) filled++;
}

float PerformancePanel::PassHistory::Avg(size_t window) const {
	if (filled == 0) return 0.0f;
	const size_t n = std::min(window, filled);
	float sum = 0.0f;
	for (size_t i = 0; i < n; i++) {
		size_t idx = (offset + PerformancePanel::HISTORY_SIZE - 1 - i) % PerformancePanel::HISTORY_SIZE;
		sum += samples[idx];
	}
	return sum / static_cast<float>(n);
}

float PerformancePanel::PassHistory::Max(size_t window) const {
	if (filled == 0) return 0.0f;
	const size_t n = std::min(window, filled);
	float m = 0.0f;
	for (size_t i = 0; i < n; i++) {
		size_t idx = (offset + PerformancePanel::HISTORY_SIZE - 1 - i) % PerformancePanel::HISTORY_SIZE;
		m = std::max(m, samples[idx]);
	}
	return m;
}

// 12 well-separated hues, repeating. Tuned to feel like a developer's
// distinct-color set rather than a rainbow — chosen for category contrast at
// small thumbnails.
uint32_t PerformancePanel::ColorForIndex(size_t idx) const {
	static const uint32_t kPalette[12] = {
		IM_COL32( 78, 154, 241, 220), // blue
		IM_COL32( 78, 200, 130, 220), // green
		IM_COL32(232, 162,  74, 220), // amber
		IM_COL32(212,  92, 158, 220), // pink
		IM_COL32(168, 138, 232, 220), // violet
		IM_COL32( 86, 198, 198, 220), // teal
		IM_COL32(232, 200, 100, 220), // yellow
		IM_COL32(124, 196, 110, 220), // lime
		IM_COL32(220, 130, 120, 220), // salmon
		IM_COL32(118, 158, 198, 220), // slate
		IM_COL32(196, 124, 200, 220), // mauve
		IM_COL32(102, 200, 156, 220), // mint
	};
	return kPalette[idx % 12];
}

void PerformancePanel::Update(float fps, float gpuTimeMs, float frameTimeMs) {
	m_fps = fps;
	m_gpu_ms = gpuTimeMs;
	m_frame_ms = frameTimeMs;

	m_frame_times[m_offset] = frameTimeMs;
	m_gpu_times[m_offset]   = gpuTimeMs;
	m_offset = (m_offset + 1) % HISTORY_SIZE;

	// Push per-pass samples into pass-history ring buffers, keyed by name.
	if (m_snapshot && m_metrics) {
		const auto& passes = m_snapshot->passes;
		const auto& times  = m_metrics->passTimesMs;
		for (size_t i = 0; i < passes.size() && i < times.size(); i++) {
			auto& h = m_pass_history[passes[i].name];
			if (h.color == 0) h.color = ColorForIndex(i);
			h.Push(times[i]);
		}
	}
}

// Stable axis update — grow fast (with headroom) on a new peak; decay slowly
// otherwise. Floors at `min_floor` so an idle scene still renders the chart at
// a meaningful scale. EMA factor 0.995 ≈ 2.3s half-life at 60Hz.
static void UpdateSoftMax(float& axis, float current_peak, float min_floor) {
	const float headroom = 1.20f;
	if (current_peak * 1.10f > axis) {
		axis = current_peak * headroom;
	} else {
		axis = axis * 0.995f + current_peak * 0.005f;
	}
	if (axis < min_floor) axis = min_floor;
}

void PerformancePanel::DrawBudgetStrip(float height) {
	const float width = ImGui::GetContentRegionAvail().x;
	if (width < 8.0f || height < 8.0f) return;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	auto* dl = ImGui::GetWindowDrawList();

	// Always render the chassis so the strip is visible even with no passes.
	dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
		UIStyle::U32(UIStyle::kBgLight), 2.0f);

	if (!m_snapshot || !m_metrics) {
		ImGui::Dummy(ImVec2(width, height));
		return;
	}

	const auto& passes = m_snapshot->passes;
	const auto& times  = m_metrics->passTimesMs;

	float total = 0.0f;
	for (size_t i = 0; i < passes.size() && i < times.size(); i++) total += times[i];
	const float fence_ms = (m_gpu_ms > 0.0f) ? m_gpu_ms : total;
	const float current  = std::max(total, fence_ms);

	UpdateSoftMax(m_axis_budget, current, 16.6f);
	const float axis_max = m_axis_budget;

	// Clip everything we draw past the strip — keeps segments + guide labels
	// inside the panel even when the user shrinks it.
	dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);

	float x = origin.x;
	int hovered = -1;
	for (size_t i = 0; i < passes.size() && i < times.size(); i++) {
		float seg = (times[i] / axis_max) * width;
		if (seg < 0.5f) continue;

		ImVec2 a(x, origin.y + 1.5f);
		ImVec2 b(std::min(x + seg - 0.5f, origin.x + width - 1.0f), origin.y + height - 1.5f);
		if (b.x <= a.x) { x += seg; continue; }

		auto it = m_pass_history.find(passes[i].name);
		ImU32 col = it != m_pass_history.end() ? it->second.color : ColorForIndex(i);
		dl->AddRectFilled(a, b, col, 1.0f);
		x += seg;

		if (ImGui::IsMouseHoveringRect(a, b)) hovered = (int)i;
	}

	// Budget guides — only draw if the line actually fits inside the strip.
	auto draw_guide = [&](float ms, ImVec4 c, const char* label) {
		if (axis_max <= ms) return;
		float gx = origin.x + (ms / axis_max) * width;
		dl->AddLine(ImVec2(gx, origin.y + 1), ImVec2(gx, origin.y + height - 1),
			UIStyle::U32(UIStyle::Alpha(c, 0.55f)), 1.0f);
		// Tiny label, only if there's room
		if (gx + 24 < origin.x + width) {
			ImGui::PushFont(UIStyle::FontMonoDetail());
			dl->AddText(ImVec2(gx + 2, origin.y + 1),
				UIStyle::U32(UIStyle::Alpha(c, 0.7f)), label);
			ImGui::PopFont();
		}
	};
	draw_guide(16.6f, UIStyle::kBudgetWarn, "60");
	draw_guide(33.3f, UIStyle::kBudgetOver, "30");

	dl->PopClipRect();
	ImGui::Dummy(ImVec2(width, height));

	if (hovered >= 0 && hovered < (int)passes.size()) {
		ImGui::BeginTooltip();
		ImGui::TextUnformatted(passes[hovered].name.c_str());
		char buf[64]; snprintf(buf, sizeof(buf), "%.3f ms", times[hovered]);
		UIStyle::Numeric("time", buf);
		ImGui::EndTooltip();
	}
}

void PerformancePanel::DrawStackedHistory(float height) {
	const float width = ImGui::GetContentRegionAvail().x;
	if (width < 8.0f || height < 8.0f) return;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	auto* dl = ImGui::GetWindowDrawList();

	dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
		UIStyle::U32(UIStyle::kBgLight), 2.0f);

	if (!m_snapshot) {
		ImGui::Dummy(ImVec2(width, height));
		return;
	}
	const auto& passes = m_snapshot->passes;

	// Single-pass total across the window. Same loop computes the scale
	// (current peak, fed to the soft-max) and the per-sample totals used for
	// the chart's stacked baseline.
	float current_peak = 0.0f;
	for (size_t s = 0; s < HISTORY_SIZE; s++) {
		float t = 0.0f;
		for (const auto& [name, h] : m_pass_history) t += h.samples[s];
		if (t > current_peak) current_peak = t;
	}
	UpdateSoftMax(m_axis_history, current_peak, 16.6f);
	const float axis_max = m_axis_history;

	dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);

	// 60Hz reference line at 16.6ms — drawn under the data so spikes overlay it.
	if (axis_max > 16.6f) {
		float bud_y = origin.y + height - (16.6f / axis_max) * height;
		dl->AddLine(ImVec2(origin.x, bud_y), ImVec2(origin.x + width, bud_y),
			UIStyle::U32(UIStyle::Alpha(UIStyle::kBudgetWarn, 0.30f)), 1.0f);
	}

	const size_t cur_offset = m_offset;
	const float dx = width / static_cast<float>(HISTORY_SIZE);
	std::vector<float> baseline(HISTORY_SIZE, 0.0f);

	for (size_t i = 0; i < passes.size(); i++) {
		auto it = m_pass_history.find(passes[i].name);
		if (it == m_pass_history.end()) continue;
		const auto& h = it->second;
		const ImU32 col = h.color;

		for (size_t s = 0; s + 1 < HISTORY_SIZE; s++) {
			size_t idx0 = (cur_offset + s) % HISTORY_SIZE;
			size_t idx1 = (cur_offset + s + 1) % HISTORY_SIZE;
			float v0 = h.samples[idx0];
			float v1 = h.samples[idx1];

			float x0 = origin.x + s * dx;
			float x1 = origin.x + (s + 1) * dx;
			float b0 = origin.y + height - std::min(1.0f, baseline[s]   / axis_max) * height;
			float b1 = origin.y + height - std::min(1.0f, baseline[s+1] / axis_max) * height;
			float t0 = origin.y + height - std::min(1.0f, (baseline[s]   + v0) / axis_max) * height;
			float t1 = origin.y + height - std::min(1.0f, (baseline[s+1] + v1) / axis_max) * height;

			ImVec2 quad[4] = {
				ImVec2(x0, b0), ImVec2(x1, b1), ImVec2(x1, t1), ImVec2(x0, t0)
			};
			dl->AddConvexPolyFilled(quad, 4, col);

			baseline[s]   += v0;
			baseline[s+1] += v1;
		}
	}

	// Top-right axis label
	char lbl[24]; snprintf(lbl, sizeof(lbl), "%.1f ms", axis_max);
	ImGui::PushFont(UIStyle::FontMonoDetail());
	ImVec2 ts = ImGui::CalcTextSize(lbl);
	dl->AddText(ImVec2(origin.x + width - ts.x - 4, origin.y + 2),
		UIStyle::U32(UIStyle::kTextDim), lbl);
	ImGui::PopFont();

	dl->PopClipRect();
	ImGui::Dummy(ImVec2(width, height));
}

void PerformancePanel::DrawPassTable() {
	if (!m_snapshot) {
		ImGui::TextColored(UIStyle::kTextDim, "No render graph compiled.");
		return;
	}

	const auto& passes = m_snapshot->passes;
	const auto& times  = m_metrics ? m_metrics->passTimesMs : std::vector<float>{};

	float frame_total = m_gpu_ms > 0.0f ? m_gpu_ms : 0.001f;

	ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
		| ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable
		| ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY;
	// Use the remaining content region — the table fills whatever vertical
	// space the charts above didn't claim.
	ImVec2 table_size(0.0f, std::max(80.0f, ImGui::GetContentRegionAvail().y));
	if (!ImGui::BeginTable("##passtbl", 7, flags, table_size)) return;

	ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 14.0f);
	ImGui::TableSetupColumn("Pass",   ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Q",      ImGuiTableColumnFlags_WidthFixed, 30.0f);
	ImGui::TableSetupColumn("ms",     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 56.0f);
	ImGui::TableSetupColumn("avg",    ImGuiTableColumnFlags_WidthFixed, 56.0f);
	ImGui::TableSetupColumn("max",    ImGuiTableColumnFlags_WidthFixed, 56.0f);
	ImGui::TableSetupColumn("%",      ImGuiTableColumnFlags_WidthFixed, 50.0f);
	ImGui::TableHeadersRow();

	// Build a sortable index list. ImGui's TableGetSortSpecs gives the column we
	// sorted on; we apply manually because the data isn't laid out as a vector.
	struct Row { size_t idx; float ms, avg, max, pct; };
	std::vector<Row> rows;
	rows.reserve(passes.size());
	for (size_t i = 0; i < passes.size(); i++) {
		float ms = i < times.size() ? times[i] : 0.0f;
		auto it = m_pass_history.find(passes[i].name);
		float avg = it != m_pass_history.end() ? it->second.Avg(60) : 0.0f;
		float mx  = it != m_pass_history.end() ? it->second.Max(60) : 0.0f;
		rows.push_back({ i, ms, avg, mx, (ms / frame_total) * 100.0f });
	}
	if (auto* sort = ImGui::TableGetSortSpecs()) {
		if (sort->SpecsCount > 0) {
			const auto& s = sort->Specs[0];
			std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
				bool asc = (s.SortDirection == ImGuiSortDirection_Ascending);
				switch (s.ColumnIndex) {
					case 1: return asc ? passes[a.idx].name < passes[b.idx].name
					                   : passes[a.idx].name > passes[b.idx].name;
					case 3: return asc ? a.ms  < b.ms  : a.ms  > b.ms;
					case 4: return asc ? a.avg < b.avg : a.avg > b.avg;
					case 5: return asc ? a.max < b.max : a.max > b.max;
					case 6: return asc ? a.pct < b.pct : a.pct > b.pct;
					default: return a.idx < b.idx;
				}
			});
		}
	}

	for (const auto& r : rows) {
		const auto& p = passes[r.idx];
		ImGui::TableNextRow();

		// Color swatch
		ImGui::TableSetColumnIndex(0);
		auto it = m_pass_history.find(p.name);
		ImU32 col = it != m_pass_history.end() ? it->second.color : ColorForIndex(r.idx);
		ImVec2 pos = ImGui::GetCursorScreenPos();
		float h = ImGui::GetTextLineHeight();
		ImGui::GetWindowDrawList()->AddRectFilled(
			ImVec2(pos.x + 2, pos.y + 3),
			ImVec2(pos.x + 11, pos.y + h - 1),
			col, 2.0f);

		// Name
		ImGui::TableSetColumnIndex(1);
		if (!p.enabled) ImGui::PushStyleColor(ImGuiCol_Text, UIStyle::kTextDim);
		ImGui::TextUnformatted(p.name.c_str());
		if (!p.enabled) ImGui::PopStyleColor();

		// Queue
		ImGui::TableSetColumnIndex(2);
		const bool gfx = p.affinity == QueueAffinity::Graphics;
		ImGui::TextColored(gfx ? UIStyle::kQueueGraphics : UIStyle::kQueueCompute,
			"%s", gfx ? "G" : "C");

		// Times
		auto pushMono = [] { ImGui::PushFont(UIStyle::FontMonoBody()); };
		auto popMono  = [] { ImGui::PopFont(); };

		ImGui::TableSetColumnIndex(3); pushMono(); ImGui::Text("%6.2f", r.ms);  popMono();
		ImGui::TableSetColumnIndex(4); pushMono(); ImGui::Text("%6.2f", r.avg); popMono();
		ImGui::TableSetColumnIndex(5); pushMono(); ImGui::Text("%6.2f", r.max); popMono();
		ImGui::TableSetColumnIndex(6);
		ImGui::PushStyleColor(ImGuiCol_Text, UIStyle::BudgetColor(r.pct / 100.0f, 0.40f, 0.70f));
		pushMono(); ImGui::Text("%5.1f", r.pct); popMono();
		ImGui::PopStyleColor();
	}

	ImGui::EndTable();
}

void PerformancePanel::Draw() {
	ImGui::Begin("Performance");

	// === Top-line numerics — single line, monospace, no badge clutter ===
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%4.0f", m_fps);
		UIStyle::Numeric("fps", buf);
		ImGui::SameLine(0, 12);
		snprintf(buf, sizeof(buf), "%5.2f ms", m_frame_ms);
		UIStyle::Numeric("frame", buf);
		ImGui::SameLine(0, 12);
		snprintf(buf, sizeof(buf), "%5.2f ms", m_gpu_ms);
		UIStyle::Numeric("gpu", buf);
	}

	// === Charts — adapt heights to whatever vertical room remains ===
	// Reserved table minimum keeps the numerics readable; charts share
	// remaining vertical room with a 1:3 ratio (strip thin, history fatter).
	const float avail_h        = ImGui::GetContentRegionAvail().y;
	const float reserved_table = std::min(avail_h * 0.45f, 240.0f);
	const float reserved_hdrs  = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
	float charts_h = std::max(40.0f, avail_h - reserved_table - reserved_hdrs);
	const float strip_h   = std::clamp(charts_h * 0.18f, 16.0f, 26.0f);
	const float history_h = std::max(40.0f, charts_h - strip_h - 8.0f);

	UIStyle::SectionHeader("Frame Budget");
	DrawBudgetStrip(strip_h);

	UIStyle::SectionHeader("History (4s)");
	DrawStackedHistory(history_h);

	UIStyle::SectionHeader("Passes");
	DrawPassTable();

	ImGui::End();
}
