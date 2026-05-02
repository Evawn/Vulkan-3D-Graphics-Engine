#include "MemoryPanel.h"
#include "UIStyle.h"
#include "VkFormatString.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

void MemoryPanel::SetAllocator(VmaAllocator allocator, uint32_t heapCount) {
	m_allocator = allocator;
	m_heap_count = heapCount;
	m_heap_info.assign(heapCount, HeapInfo{});
}

void MemoryPanel::Update() {
	if (m_allocator == VK_NULL_HANDLE) return;

	if (m_stats_throttle > 0) { m_stats_throttle--; return; }
	m_stats_throttle = 30; // ~2Hz at 60fps

	std::vector<VmaBudget> budgets(m_heap_count);
	vmaGetHeapBudgets(m_allocator, budgets.data());

	m_total_bytes = 0;
	for (uint32_t i = 0; i < m_heap_count; i++) {
		auto& h = m_heap_info[i];
		h.usage  = budgets[i].usage;
		h.budget = budgets[i].budget;
		h.high_water = std::max<uint64_t>(h.high_water, h.usage);

		float mb = static_cast<float>(h.usage) / (1024.0f * 1024.0f);
		h.history[h.offset] = mb;
		h.offset = (h.offset + 1) % HISTORY_SIZE;

		m_total_bytes += h.usage;
	}

	VmaTotalStatistics stats{};
	vmaCalculateStatistics(m_allocator, &stats);
	m_allocation_count = stats.total.statistics.allocationCount;
}

void MemoryPanel::DrawHeapStrip() {
	auto* dl = ImGui::GetWindowDrawList();

	for (uint32_t i = 0; i < m_heap_count; i++) {
		const auto& h = m_heap_info[i];
		// Skip empty heaps that the device exposes but never populated.
		if (h.budget == 0 && h.usage == 0) continue;

		float fraction = h.budget > 0 ? float(h.usage) / float(h.budget) : 0.0f;
		fraction = std::clamp(fraction, 0.0f, 1.0f);

		char left[64], right_str[64];
		UIStyle::FormatBytes(left,      sizeof(left),      h.usage);
		UIStyle::FormatBytes(right_str, sizeof(right_str), h.budget);
		char overlay[128];
		snprintf(overlay, sizeof(overlay), "%s / %s", left, right_str);

		// Heap label
		ImGui::TextColored(UIStyle::kTextDim, "Heap %u", i);
		ImGui::SameLine(60);

		ImVec2 pos = ImGui::GetCursorScreenPos();
		float w = ImGui::GetContentRegionAvail().x;
		float bh = ImGui::GetTextLineHeight() + 2.0f;
		if (w < 8.0f) { ImGui::Dummy(ImVec2(w, bh)); continue; }

		ImVec2 br(pos.x + w, pos.y + bh);
		dl->PushClipRect(pos, br, true);
		dl->AddRectFilled(pos, br, UIStyle::U32(UIStyle::kBgLight), 2.0f);

		ImVec4 fill = UIStyle::BudgetColor(fraction);
		dl->AddRectFilled(pos, ImVec2(pos.x + w * fraction, pos.y + bh),
			UIStyle::U32(UIStyle::Alpha(fill, 0.6f)), 2.0f);

		// High-water tick (skipped if it'd land outside the bar)
		if (h.high_water > 0 && h.budget > 0) {
			float hw = std::clamp(float(h.high_water) / float(h.budget), 0.0f, 1.0f);
			float tx = pos.x + w * hw;
			dl->AddLine(ImVec2(tx, pos.y + 1), ImVec2(tx, pos.y + bh - 1),
				UIStyle::U32(UIStyle::kAccent), 1.5f);
		}

		// Centered usage text
		ImGui::PushFont(UIStyle::FontMonoDetail());
		ImVec2 ts = ImGui::CalcTextSize(overlay);
		ImVec2 tp(pos.x + (w - ts.x) * 0.5f, pos.y + (bh - ts.y) * 0.5f);
		dl->AddText(tp, UIStyle::U32(UIStyle::kText), overlay);
		ImGui::PopFont();

		dl->PopClipRect();
		ImGui::Dummy(ImVec2(w, bh));
	}
}

// Same soft-max policy as PerformancePanel — grow fast on a peak, decay
// slowly. Floor in MB so the chart has a sensible scale even at startup
// before any allocations are reported.
static void UpdateMBSoftMax(float& axis, float current_peak, float floor_mb) {
	const float headroom = 1.20f;
	if (current_peak * 1.10f > axis) {
		axis = current_peak * headroom;
	} else {
		axis = axis * 0.998f + current_peak * 0.002f;
	}
	if (axis < floor_mb) axis = floor_mb;
}

void MemoryPanel::DrawHistoryChart(float height) {
	const float width = ImGui::GetContentRegionAvail().x;
	if (width < 8.0f || height < 8.0f) return;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	auto* dl = ImGui::GetWindowDrawList();

	dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
		UIStyle::U32(UIStyle::kBgLight), 2.0f);

	if (m_heap_count == 0) {
		ImGui::Dummy(ImVec2(width, height));
		return;
	}

	float current_peak = 0.0f;
	for (size_t s = 0; s < HISTORY_SIZE; s++) {
		float t = 0.0f;
		for (const auto& h : m_heap_info) t += h.history[s];
		if (t > current_peak) current_peak = t;
	}
	UpdateMBSoftMax(m_axis_history_mb, current_peak, 64.0f);
	const float axis_max = m_axis_history_mb;

	dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);

	const float dx = width / static_cast<float>(HISTORY_SIZE);
	std::vector<float> baseline(HISTORY_SIZE, 0.0f);

	for (uint32_t i = 0; i < m_heap_count; i++) {
		const auto& h = m_heap_info[i];
		if (h.budget == 0 && h.usage == 0) continue;

		ImVec4 col = (i == 0) ? UIStyle::kAccent : UIStyle::kQueueGraphics;
		ImU32 fill = UIStyle::U32(UIStyle::Alpha(col, 0.55f));

		for (size_t s = 0; s + 1 < HISTORY_SIZE; s++) {
			size_t idx0 = (h.offset + s) % HISTORY_SIZE;
			size_t idx1 = (h.offset + s + 1) % HISTORY_SIZE;
			float v0 = h.history[idx0];
			float v1 = h.history[idx1];

			float x0 = origin.x + s * dx;
			float x1 = origin.x + (s + 1) * dx;
			float b0 = origin.y + height - std::min(1.0f, baseline[s]   / axis_max) * height;
			float b1 = origin.y + height - std::min(1.0f, baseline[s+1] / axis_max) * height;
			float t0 = origin.y + height - std::min(1.0f, (baseline[s]   + v0) / axis_max) * height;
			float t1 = origin.y + height - std::min(1.0f, (baseline[s+1] + v1) / axis_max) * height;

			ImVec2 quad[4] = {
				ImVec2(x0, b0), ImVec2(x1, b1), ImVec2(x1, t1), ImVec2(x0, t0)
			};
			dl->AddConvexPolyFilled(quad, 4, fill);

			baseline[s]   += v0;
			baseline[s+1] += v1;
		}
	}

	char lbl[24]; snprintf(lbl, sizeof(lbl), "%.0f MB", axis_max);
	ImGui::PushFont(UIStyle::FontMonoDetail());
	ImVec2 ts = ImGui::CalcTextSize(lbl);
	dl->AddText(ImVec2(origin.x + width - ts.x - 4, origin.y + 2),
		UIStyle::U32(UIStyle::kTextDim), lbl);
	ImGui::PopFont();

	dl->PopClipRect();
	ImGui::Dummy(ImVec2(width, height));
}

static uint64_t ImageBytesEstimate(const ImageDesc& d) {
	// Rough format-size table; matches common Vulkan formats used in this engine.
	auto bpp = [](VkFormat f) -> uint64_t {
		switch (f) {
			case VK_FORMAT_R8_UNORM:               return 1;
			case VK_FORMAT_D16_UNORM:              return 2;
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB:
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB:
			case VK_FORMAT_R32_SFLOAT:
			case VK_FORMAT_D32_SFLOAT:
			case VK_FORMAT_D24_UNORM_S8_UINT:      return 4;
			case VK_FORMAT_D32_SFLOAT_S8_UINT:     return 5;
			case VK_FORMAT_R16G16B16A16_SFLOAT:    return 8;
			case VK_FORMAT_R32G32B32A32_SFLOAT:    return 16;
			default:                               return 4;
		}
	};
	uint64_t pixels = uint64_t(d.width) * d.height * d.depth;
	uint64_t samples = uint64_t(d.samples == 0 ? 1 : d.samples);
	return pixels * bpp(d.format) * samples;
}

void MemoryPanel::DrawResourceTable(float height) {
	if (!m_snapshot) {
		ImGui::TextColored(UIStyle::kTextDim, "No render graph compiled.");
		return;
	}

	uint64_t total = 0;
	for (const auto& img : m_snapshot->images)  total += ImageBytesEstimate(img.desc);
	for (const auto& buf : m_snapshot->buffers) total += buf.desc.size;

	char total_str[64]; UIStyle::FormatBytes(total_str, sizeof(total_str), total);
	UIStyle::Numeric("graph total", total_str);

	ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
		| ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable
		| ImGuiTableFlags_ScrollY;

	if (!ImGui::BeginTable("##resources", 5, flags, ImVec2(0, std::max(80.0f, height)))) return;

	ImGui::TableSetupColumn("Kind",   ImGuiTableColumnFlags_WidthFixed, 36.0f);
	ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Bytes",  ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 76.0f);
	ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 60.0f);
	ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableHeadersRow();

	struct Row {
		bool image;
		std::string name;
		uint64_t bytes;
		bool imported;
		std::string detail;
		ImVec4 color;
	};
	std::vector<Row> rows;
	rows.reserve(m_snapshot->images.size() + m_snapshot->buffers.size());

	for (const auto& img : m_snapshot->images) {
		Row r;
		r.image = true;
		r.name = img.name;
		r.bytes = ImageBytesEstimate(img.desc);
		r.imported = img.imported;
		char buf[96];
		snprintf(buf, sizeof(buf), "%ux%u %s %s",
			img.desc.width, img.desc.height,
			VkStr::Format(img.desc.format),
			VkStr::SampleCount(img.desc.samples));
		r.detail = buf;
		r.color = UIStyle::kResourceImage;
		rows.push_back(std::move(r));
	}
	for (const auto& buf : m_snapshot->buffers) {
		Row r;
		r.image = false;
		r.name = buf.name;
		r.bytes = buf.desc.size;
		r.imported = buf.imported;
		r.detail = (buf.desc.lifetime == Lifetime::Persistent) ? "persistent" : "transient";
		r.color = UIStyle::kResourceBuffer;
		rows.push_back(std::move(r));
	}

	if (auto* sort = ImGui::TableGetSortSpecs()) {
		if (sort->SpecsCount > 0) {
			const auto& s = sort->Specs[0];
			std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
				bool asc = (s.SortDirection == ImGuiSortDirection_Ascending);
				switch (s.ColumnIndex) {
					case 0: return asc ? a.image > b.image : a.image < b.image;
					case 1: return asc ? a.name < b.name : a.name > b.name;
					case 2: return asc ? a.bytes < b.bytes : a.bytes > b.bytes;
					case 3: return asc ? a.imported < b.imported : a.imported > b.imported;
					default: return a.bytes > b.bytes;
				}
			});
		}
	}

	for (const auto& r : rows) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextColored(r.color, r.image ? "img" : "buf");
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted(r.name.c_str());
		ImGui::TableSetColumnIndex(2);
		char b[32]; UIStyle::FormatBytes(b, sizeof(b), r.bytes);
		ImGui::PushFont(UIStyle::FontMonoBody());
		ImGui::TextUnformatted(b);
		ImGui::PopFont();
		ImGui::TableSetColumnIndex(3);
		ImGui::TextColored(r.imported ? UIStyle::kResourceImport : UIStyle::kTextDim,
			r.imported ? "import" : "alloc");
		ImGui::TableSetColumnIndex(4);
		ImGui::TextColored(UIStyle::kTextDim, "%s", r.detail.c_str());
	}

	ImGui::EndTable();
}

void MemoryPanel::Draw() {
	ImGui::Begin("Memory");

	{
		char total[64]; UIStyle::FormatBytes(total, sizeof(total), m_total_bytes);
		UIStyle::Numeric("resident", total);
		ImGui::SameLine(0, 12);
		char a[16]; snprintf(a, sizeof(a), "%u", m_allocation_count);
		UIStyle::Numeric("allocs", a);
	}

	UIStyle::SectionHeader("Heaps");
	DrawHeapStrip();

	// Reserve a chunk of remaining vertical space for the resource table; give
	// the history chart whatever's left, capped to a sensible band so it
	// doesn't dominate when the panel is tall.
	const float avail_h    = ImGui::GetContentRegionAvail().y;
	const float reserved   = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
	const float table_h    = std::min(std::max(avail_h * 0.42f, 120.0f), 320.0f);
	const float history_h  = std::clamp(avail_h - table_h - reserved, 36.0f, 110.0f);

	UIStyle::SectionHeader("History (MB)");
	DrawHistoryChart(history_h);

	UIStyle::SectionHeader("Resources");
	DrawResourceTable(table_h);

	ImGui::End();
}
