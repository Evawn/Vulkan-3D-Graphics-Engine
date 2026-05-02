#include "RenderGraphPanel.h"
#include "RenderItem.h"
#include "UIStyle.h"
#include "VkFormatString.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <queue>
#include <vector>
#include <unordered_map>

// =============================================================================
// DAG canvas — Topological levels are computed in-panel from the snapshot's
// read/write handle lists. For each pass A and downstream pass B, an edge
// A → B exists when A writes a handle B reads. Pass level = 1 + max(level of
// every pass that produces something this pass consumes); roots are level 0.
//
// Layout: pass nodes are placed left-to-right by level, top-to-bottom inside
// a level. Graphics passes occupy the upper band; AsyncCompute the lower band.
// Edges are bezier curves drawn with ImDrawList — no third-party node-graph
// library required.
// =============================================================================

namespace {

struct NodeLayout {
	int level = 0;
	int rowInLevel = 0;
	ImVec2 pos;          // top-left in canvas-space (pre-pan/zoom)
	ImVec2 size;
	ImU32  fill;
	ImU32  border;
};

struct Edge {
	size_t from;   // pass index
	size_t to;     // pass index
	uint32_t handleId;
	bool isImage;
	bool crossStream;
};

// Build edges from read/write handle overlap. Cost is O(P * R) where P passes
// and R reads-per-pass; tens of passes, single-digit reads, well below frame
// budget.
static std::vector<Edge> BuildEdges(const GraphSnapshot& snap) {
	// Last-writer index per resource id
	std::unordered_map<uint32_t, size_t> lastImageWriter;
	std::unordered_map<uint32_t, size_t> lastBufferWriter;

	std::vector<Edge> out;

	for (size_t i = 0; i < snap.passes.size(); i++) {
		const auto& p = snap.passes[i];

		for (auto h : p.readImages) {
			auto it = lastImageWriter.find(h.id);
			if (it != lastImageWriter.end()) {
				Edge e{ it->second, i, h.id, true, false };
				e.crossStream = (snap.passes[it->second].affinity != p.affinity);
				out.push_back(e);
			}
		}
		for (auto h : p.readBuffers) {
			auto it = lastBufferWriter.find(h.id);
			if (it != lastBufferWriter.end()) {
				Edge e{ it->second, i, h.id, false, false };
				e.crossStream = (snap.passes[it->second].affinity != p.affinity);
				out.push_back(e);
			}
		}

		for (auto h : p.writeImages)  lastImageWriter [h.id] = i;
		for (auto h : p.writeBuffers) lastBufferWriter[h.id] = i;
	}
	return out;
}

// Per-pass topological level via Kahn-like relaxation in declaration order.
// Identical to BFS-from-roots for a DAG laid out in topo order.
static std::vector<int> ComputeLevels(const GraphSnapshot& snap, const std::vector<Edge>& edges) {
	std::vector<int> level(snap.passes.size(), 0);
	for (const auto& e : edges) {
		level[e.to] = std::max(level[e.to], level[e.from] + 1);
	}
	return level;
}

} // anonymous

void RenderGraphPanel::DrawCanvas() {
	if (!m_snapshot || m_snapshot->passes.empty()) {
		ImGui::TextColored(UIStyle::kTextDim, "No render graph compiled.");
		return;
	}

	const auto& snap = *m_snapshot;
	auto edges = BuildEdges(snap);
	auto levels = ComputeLevels(snap, edges);

	const int max_level = levels.empty() ? 0 : *std::max_element(levels.begin(), levels.end());

	// Group passes by (level, affinity) so we can stack them. Order within a
	// level matches declaration order — stable enough for visual scanning.
	std::vector<std::vector<size_t>> col_gfx(max_level + 1);
	std::vector<std::vector<size_t>> col_cmp(max_level + 1);
	for (size_t i = 0; i < snap.passes.size(); i++) {
		auto& bucket = (snap.passes[i].affinity == QueueAffinity::AsyncCompute)
			? col_cmp[levels[i]]
			: col_gfx[levels[i]];
		bucket.push_back(i);
	}

	// Layout pass: positions in canvas-space units. Compute first so we can
	// fit bounds into the viewport.
	std::vector<NodeLayout> nodes(snap.passes.size());
	const float kNodeW = 160.0f;
	const float kNodeH = 48.0f;
	const float kColGap = 60.0f;
	const float kRowGap = 14.0f;
	const float kStreamGap = 32.0f;

	float x = 12.0f;
	float gfx_y0 = 12.0f;
	for (int lv = 0; lv <= max_level; lv++) {
		float y = gfx_y0;
		for (size_t idx : col_gfx[lv]) {
			nodes[idx].pos  = ImVec2(x, y);
			nodes[idx].size = ImVec2(kNodeW, kNodeH);
			y += kNodeH + kRowGap;
		}
		// async-compute lane below — offset more so the bands are visually distinct
		float y2 = std::max(y, gfx_y0 + 2 * (kNodeH + kRowGap)) + kStreamGap;
		for (size_t idx : col_cmp[lv]) {
			nodes[idx].pos  = ImVec2(x, y2);
			nodes[idx].size = ImVec2(kNodeW, kNodeH);
			y2 += kNodeH + kRowGap;
		}
		x += kNodeW + kColGap;
	}

	// Toolbar: mode toggle + zoom controls
	{
		const char* mode_lbl = (m_mode == Mode::Graph) ? "Graph" : "Table";
		if (ImGui::SmallButton(mode_lbl)) {
			m_mode = (m_mode == Mode::Graph) ? Mode::Table : Mode::Graph;
		}
		ImGui::SameLine(0, 8);
		if (ImGui::SmallButton("-")) m_zoom = std::max(0.4f, m_zoom - 0.1f);
		ImGui::SameLine(0, 4);
		ImGui::Text("%.0f%%", m_zoom * 100.0f);
		ImGui::SameLine(0, 4);
		if (ImGui::SmallButton("+")) m_zoom = std::min(2.0f, m_zoom + 0.1f);
		ImGui::SameLine(0, 8);
		if (ImGui::SmallButton("Fit")) { m_pan = ImVec2(0, 0); m_zoom = 1.0f; }
	}

	// Canvas region
	ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
	ImVec2 canvas_size   = ImGui::GetContentRegionAvail();
	if (canvas_size.y < 200) canvas_size.y = 200;

	auto* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(canvas_origin,
		ImVec2(canvas_origin.x + canvas_size.x, canvas_origin.y + canvas_size.y),
		UIStyle::U32(ImVec4(0.07f, 0.07f, 0.075f, 1.0f)), 3.0f);

	// Capture pan via background-button drag.
	ImGui::InvisibleButton("##canvas_bg", canvas_size);
	const bool canvas_hovered = ImGui::IsItemHovered();
	if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
		ImVec2 d = ImGui::GetIO().MouseDelta;
		m_pan.x += d.x;
		m_pan.y += d.y;
	}
	if (canvas_hovered && ImGui::GetIO().MouseWheel != 0.0f) {
		float prev = m_zoom;
		m_zoom = std::clamp(m_zoom + ImGui::GetIO().MouseWheel * 0.1f, 0.4f, 2.0f);
		(void)prev; // could re-anchor on cursor; keep simple for now
	}

	// Local lambda: project a canvas-space point into screen space.
	auto P = [&](ImVec2 p) -> ImVec2 {
		return ImVec2(canvas_origin.x + m_pan.x + p.x * m_zoom,
		              canvas_origin.y + m_pan.y + p.y * m_zoom);
	};

	// Stream lane labels in the canvas background — light hint that the y axis
	// has semantic meaning.
	{
		float async_y = canvas_origin.y + m_pan.y + (gfx_y0 + 2 * (kNodeH + kRowGap) + kStreamGap * 0.5f) * m_zoom;
		dl->AddLine(
			ImVec2(canvas_origin.x, async_y),
			ImVec2(canvas_origin.x + canvas_size.x, async_y),
			UIStyle::U32(UIStyle::Alpha(UIStyle::kQueueCompute, 0.15f)), 1.0f);
		dl->AddText(ImVec2(canvas_origin.x + 6, async_y - 14),
			UIStyle::U32(UIStyle::Alpha(UIStyle::kQueueGraphics, 0.6f)), "graphics");
		dl->AddText(ImVec2(canvas_origin.x + 6, async_y + 2),
			UIStyle::U32(UIStyle::Alpha(UIStyle::kQueueCompute, 0.6f)), "async-compute");
	}

	// === Draw edges (under nodes) ===
	for (const auto& e : edges) {
		ImVec2 a = ImVec2(nodes[e.from].pos.x + nodes[e.from].size.x,
		                  nodes[e.from].pos.y + nodes[e.from].size.y * 0.5f);
		ImVec2 b = ImVec2(nodes[e.to].pos.x,
		                  nodes[e.to].pos.y + nodes[e.to].size.y * 0.5f);
		ImVec2 sa = P(a);
		ImVec2 sb = P(b);

		float dx = std::max(40.0f, (sb.x - sa.x) * 0.5f);
		ImVec4 col = e.isImage ? UIStyle::kResourceImage : UIStyle::kResourceBuffer;
		ImU32 cu = UIStyle::U32(UIStyle::Alpha(col, 0.85f));

		if (e.crossStream) {
			// Dashed for cross-stream handoffs.
			const int N = 16;
			for (int i = 0; i < N; i++) {
				if ((i & 1) == 0) continue;
				float t0 = float(i)     / N;
				float t1 = float(i + 1) / N;
				ImVec2 p0 = ImBezierCubicCalc(sa,
					ImVec2(sa.x + dx, sa.y), ImVec2(sb.x - dx, sb.y), sb, t0);
				ImVec2 p1 = ImBezierCubicCalc(sa,
					ImVec2(sa.x + dx, sa.y), ImVec2(sb.x - dx, sb.y), sb, t1);
				dl->AddLine(p0, p1, cu, 1.5f);
			}
		} else {
			dl->AddBezierCubic(sa,
				ImVec2(sa.x + dx, sa.y), ImVec2(sb.x - dx, sb.y), sb,
				cu, 1.4f);
		}

		// Tiny arrowhead at b.
		ImVec2 arrow_a = ImVec2(sb.x - 6, sb.y - 4);
		ImVec2 arrow_b = ImVec2(sb.x - 6, sb.y + 4);
		dl->AddTriangleFilled(arrow_a, arrow_b, sb, cu);
	}

	// === Draw nodes ===
	for (size_t i = 0; i < snap.passes.size(); i++) {
		const auto& p = snap.passes[i];
		ImVec2 a = P(nodes[i].pos);
		ImVec2 b = ImVec2(a.x + nodes[i].size.x * m_zoom,
		                  a.y + nodes[i].size.y * m_zoom);

		ImU32 fill = UIStyle::U32(p.enabled ? UIStyle::kBgLight : UIStyle::Darken(UIStyle::kBgLight, 0.6f));
		ImU32 border = UIStyle::U32(m_selectedPass == (int)i ? UIStyle::kAccent : UIStyle::kBorder);
		dl->AddRectFilled(a, b, fill, 4.0f);
		dl->AddRect(a, b, border, 4.0f, 0, m_selectedPass == (int)i ? 2.0f : 1.0f);

		// Queue band on the left
		ImVec4 qcol = (p.affinity == QueueAffinity::AsyncCompute)
			? UIStyle::kQueueCompute : UIStyle::kQueueGraphics;
		dl->AddRectFilled(a, ImVec2(a.x + 4 * m_zoom, b.y),
			UIStyle::U32(qcol), 4.0f);

		// Pass name + GPU time
		ImGui::PushFont(UIStyle::FontBody());
		dl->AddText(ImVec2(a.x + 10 * m_zoom, a.y + 4 * m_zoom),
			UIStyle::U32(p.enabled ? UIStyle::kText : UIStyle::kTextDim),
			p.name.c_str());
		ImGui::PopFont();

		float ms = (m_metrics && i < m_metrics->passTimesMs.size()) ? m_metrics->passTimesMs[i] : 0.0f;
		char buf[32]; snprintf(buf, sizeof(buf), "%.2f ms", ms);
		ImGui::PushFont(UIStyle::FontMonoDetail());
		dl->AddText(ImVec2(a.x + 10 * m_zoom, b.y - 18 * m_zoom),
			UIStyle::U32(UIStyle::kTextDim), buf);
		ImGui::PopFont();

		// Type label on the right
		const char* type_lbl = (p.type == PassType::Compute) ? "compute" : "graphics";
		ImVec2 ts = ImGui::CalcTextSize(type_lbl);
		ImGui::PushFont(UIStyle::FontMonoDetail());
		dl->AddText(ImVec2(b.x - ts.x - 8 * m_zoom, a.y + 4 * m_zoom),
			UIStyle::U32(UIStyle::Alpha(qcol, 0.7f)), type_lbl);
		ImGui::PopFont();

		// Click hit-test
		if (ImGui::IsMouseHoveringRect(a, b) && canvas_hovered) {
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				m_selectedPass = (m_selectedPass == (int)i) ? -1 : (int)i;
			}
		}
	}
}

void RenderGraphPanel::DrawTable() {
	if (!m_snapshot) return;
	const auto& snap = *m_snapshot;

	// Mode toggle on the same line as the table header so the Graph<->Table
	// interaction mirrors the canvas's button.
	if (ImGui::SmallButton("Graph")) m_mode = Mode::Graph;

	float maxTime = 0.01f;
	if (m_metrics) for (float t : m_metrics->passTimesMs) maxTime = std::max(maxTime, t);

	ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
		| ImGuiTableFlags_SizingFixedFit;
	if (!ImGui::BeginTable("##passes", 5, flags)) return;
	ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 24.0f);
	ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Q",    ImGuiTableColumnFlags_WidthFixed, 30.0f);
	ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.0f);
	ImGui::TableSetupColumn("ms",   ImGuiTableColumnFlags_WidthFixed, 130.0f);
	ImGui::TableHeadersRow();

	for (size_t i = 0; i < snap.passes.size(); i++) {
		const auto& p = snap.passes[i];
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextColored(UIStyle::kTextDim, "%zu", i);
		ImGui::TableSetColumnIndex(1);
		bool sel = m_selectedPass == (int)i;
		char lbl[128];
		snprintf(lbl, sizeof(lbl), "%s##pr%zu", p.name.c_str(), i);
		if (!p.enabled) ImGui::PushStyleColor(ImGuiCol_Text, UIStyle::kTextDim);
		if (ImGui::Selectable(lbl, sel, ImGuiSelectableFlags_SpanAllColumns)) {
			m_selectedPass = sel ? -1 : (int)i;
		}
		if (!p.enabled) ImGui::PopStyleColor();

		ImGui::TableSetColumnIndex(2);
		bool gfx = p.affinity == QueueAffinity::Graphics;
		ImGui::TextColored(gfx ? UIStyle::kQueueGraphics : UIStyle::kQueueCompute,
			"%s", gfx ? "G" : "C");

		ImGui::TableSetColumnIndex(3);
		ImGui::TextColored(p.type == PassType::Compute ? UIStyle::kQueueCompute : UIStyle::kText,
			"%s", p.type == PassType::Compute ? "Compute" : "Graphics");

		ImGui::TableSetColumnIndex(4);
		float ms = (m_metrics && i < m_metrics->passTimesMs.size()) ? m_metrics->passTimesMs[i] : 0.0f;
		ImGui::PushFont(UIStyle::FontMonoBody());
		ImGui::Text("%5.2f", ms);
		ImGui::PopFont();
		ImGui::SameLine();
		ImVec2 cursor = ImGui::GetCursorScreenPos();
		float bw = 60.0f;
		float bh = ImGui::GetTextLineHeight();
		ImGui::GetWindowDrawList()->AddRectFilled(
			cursor, ImVec2(cursor.x + bw * (ms / maxTime), cursor.y + bh),
			UIStyle::U32(p.type == PassType::Compute ? UIStyle::kQueueCompute : UIStyle::kQueueGraphics),
			2.0f);
		ImGui::Dummy(ImVec2(bw, bh));
	}
	ImGui::EndTable();
}

static const char* ImageNameOf(const GraphSnapshot& snap, ImageHandle h) {
	if (h.id < snap.images.size()) return snap.images[h.id].name.c_str();
	return "<?>";
}
static const char* BufferNameOf(const GraphSnapshot& snap, BufferHandle h) {
	if (h.id < snap.buffers.size()) return snap.buffers[h.id].name.c_str();
	return "<?>";
}

void RenderGraphPanel::DrawSelectedDetail() {
	if (!m_snapshot || m_selectedPass < 0 ||
	    m_selectedPass >= (int)m_snapshot->passes.size()) return;

	const auto& snap = *m_snapshot;
	const auto& p = snap.passes[m_selectedPass];

	UIStyle::SectionHeader(p.name.c_str());

	ImGui::TextColored(UIStyle::kTextDim, "type %s · queue %s · %s",
		p.type == PassType::Compute ? "Compute" : "Graphics",
		p.affinity == QueueAffinity::AsyncCompute ? "AsyncCompute" : "Graphics",
		p.enabled ? "enabled" : "disabled");

	if (m_metrics && m_selectedPass < (int)m_metrics->passTimesMs.size()) {
		char buf[32]; snprintf(buf, sizeof(buf), "%.3f ms", m_metrics->passTimesMs[m_selectedPass]);
		UIStyle::Numeric("gpu", buf);
	}

	if (!p.readImages.empty() || !p.readBuffers.empty()) {
		ImGui::SeparatorText("Reads");
		for (auto h : p.readImages)
			ImGui::BulletText("img  %s", ImageNameOf(snap, h));
		for (auto h : p.readBuffers)
			ImGui::BulletText("buf  %s", BufferNameOf(snap, h));
	}
	if (!p.writeImages.empty() || !p.writeBuffers.empty()) {
		ImGui::SeparatorText("Writes");
		for (auto h : p.writeImages)
			ImGui::BulletText("img  %s", ImageNameOf(snap, h));
		for (auto h : p.writeBuffers)
			ImGui::BulletText("buf  %s", BufferNameOf(snap, h));
	}

	if (p.gfx) {
		const auto& gfx = *p.gfx;
		ImGui::SeparatorText("Attachments");
		for (size_t ci = 0; ci < gfx.colorAttachments.size(); ci++) {
			const auto& ca = gfx.colorAttachments[ci];
			ImGui::BulletText("Color[%zu] %s  (%s -> %s)",
				ci, ImageNameOf(snap, ca.target),
				VkStr::LoadOpStr(ca.load), VkStr::StoreOpStr(ca.store));
		}
		if (gfx.hasDepth) {
			ImGui::BulletText("Depth %s  (%s -> %s)",
				ImageNameOf(snap, gfx.depthTarget),
				VkStr::LoadOpStr(gfx.depthLoad), VkStr::StoreOpStr(gfx.depthStore));
		}
		if (gfx.hasResolve) {
			ImGui::BulletText("Resolve %s", ImageNameOf(snap, gfx.resolveTarget));
		}
	}

	if (m_selectedPass < (int)snap.barriers.size()) {
		const auto& b = snap.barriers[m_selectedPass];
		if (!b.imageBarriers.empty() || !b.bufferBarriers.empty()) {
			ImGui::SeparatorText("Barriers");
			for (const auto& ib : b.imageBarriers) {
				ImGui::TextColored(UIStyle::kTextDim,
					"  %s: %s -> %s",
					ImageNameOf(snap, ib.image),
					VkStr::ImageLayout(ib.oldLayout),
					VkStr::ImageLayout(ib.newLayout));
			}
			for (const auto& bb : b.bufferBarriers) {
				ImGui::TextColored(UIStyle::kTextDim,
					"  %s",
					BufferNameOf(snap, bb.buffer));
			}
		}
	}
}

void RenderGraphPanel::Draw() {
	ImGui::Begin("Render Graph");

	// Layout: split detail to the right when something is selected.
	const bool has_selection = m_selectedPass >= 0 &&
		m_snapshot && m_selectedPass < (int)m_snapshot->passes.size();

	float right_w = has_selection ? std::min(280.0f, ImGui::GetContentRegionAvail().x * 0.4f) : 0.0f;
	float left_w  = ImGui::GetContentRegionAvail().x - right_w - (has_selection ? 8.0f : 0.0f);

	ImGui::BeginChild("##rg_main", ImVec2(left_w, 0), false);
	if (m_mode == Mode::Graph) DrawCanvas();
	else                       DrawTable();
	ImGui::EndChild();

	if (has_selection) {
		ImGui::SameLine();
		ImGui::BeginChild("##rg_detail", ImVec2(right_w, 0), true);
		DrawSelectedDetail();
		ImGui::EndChild();
	}

	ImGui::End();
}
