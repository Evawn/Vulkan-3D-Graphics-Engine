#include "RenderGraphPanel.h"
#include "UIStyle.h"
#include "VkFormatString.h"
#include <algorithm>

// Helper to get a resource name by image handle
static const char* ImageName(const GraphSnapshot& snap, ImageHandle h) {
	if (h.id < snap.images.size()) return snap.images[h.id].name.c_str();
	return "<?>";
}

static const char* BufferName(const GraphSnapshot& snap, BufferHandle h) {
	if (h.id < snap.buffers.size()) return snap.buffers[h.id].name.c_str();
	return "<?>";
}

static void DrawImageRef(const GraphSnapshot& snap, ImageHandle h) {
	if (h.id >= snap.images.size()) return;
	const auto& img = snap.images[h.id];
	ImGui::BulletText("%s  (%ux%u, %s, %s)",
		img.name.c_str(),
		img.desc.width, img.desc.height,
		VkStr::Format(img.desc.format),
		VkStr::SampleCount(img.desc.samples));
}

static void DrawBufferRef(const GraphSnapshot& snap, BufferHandle h) {
	if (h.id >= snap.buffers.size()) return;
	const auto& buf = snap.buffers[h.id];
	ImGui::BulletText("%s  (%llu bytes)", buf.name.c_str(), (unsigned long long)buf.desc.size);
}

void RenderGraphPanel::Draw() {
	ImGui::Begin("Render Graph");

	if (!m_snapshot || m_snapshot->passes.empty()) {
		ImGui::TextColored(UIStyle::kTextDim, "No render graph compiled");
		ImGui::End();
		return;
	}

	const auto& snap = *m_snapshot;

	// ---- Pass Timeline ----
	if (ImGui::CollapsingHeader("Pass Timeline", ImGuiTreeNodeFlags_DefaultOpen)) {

		// Find max pass time for scaling bars
		float maxTime = 0.01f; // avoid div by zero
		if (m_metrics) {
			for (float t : m_metrics->passTimesMs)
				maxTime = std::max(maxTime, t);
		}

		ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
			| ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##passes", 4, tableFlags)) {
			ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 24.0f);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 64.0f);
			ImGui::TableSetupColumn("GPU",  ImGuiTableColumnFlags_WidthFixed, 120.0f);
			ImGui::TableHeadersRow();

			for (size_t i = 0; i < snap.passes.size(); i++) {
				const auto& pass = snap.passes[i];
				ImGui::TableNextRow();

				// Index
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(UIStyle::kTextDim, "%zu", i);

				// Name (selectable)
				ImGui::TableSetColumnIndex(1);
				bool selected = (m_selectedPass == static_cast<int>(i));
				char label[128];
				snprintf(label, sizeof(label), "%s##pass_%zu", pass.name.c_str(), i);
				if (!pass.enabled) ImGui::PushStyleColor(ImGuiCol_Text, UIStyle::kTextDim);
				if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns)) {
					m_selectedPass = selected ? -1 : static_cast<int>(i);
				}
				if (!pass.enabled) ImGui::PopStyleColor();

				// Type
				ImGui::TableSetColumnIndex(2);
				if (pass.type == PassType::Compute) {
					ImGui::TextColored(UIStyle::kAccent, "Compute");
				} else {
					ImGui::Text("Graphics");
				}

				// GPU time + bar
				ImGui::TableSetColumnIndex(3);
				float passTime = 0.0f;
				if (m_metrics && i < m_metrics->passTimesMs.size())
					passTime = m_metrics->passTimesMs[i];

				ImGui::Text("%.3fms", passTime);
				ImGui::SameLine();

				// Draw timing bar
				float barFrac = passTime / maxTime;
				ImVec2 cursor = ImGui::GetCursorScreenPos();
				float barWidth = 50.0f;
				float barHeight = ImGui::GetTextLineHeight();
				ImGui::GetWindowDrawList()->AddRectFilled(
					cursor,
					ImVec2(cursor.x + barWidth * barFrac, cursor.y + barHeight),
					pass.type == PassType::Compute
						? IM_COL32(0, 200, 60, 180)
						: IM_COL32(60, 140, 200, 180),
					2.0f);
				ImGui::Dummy(ImVec2(barWidth, barHeight));
			}
			ImGui::EndTable();
		}
	}

	// ---- Pass Detail ----
	if (m_selectedPass >= 0 && m_selectedPass < static_cast<int>(snap.passes.size())) {
		const auto& pass = snap.passes[m_selectedPass];
		char headerBuf[128];
		snprintf(headerBuf, sizeof(headerBuf), "Pass Detail: %s###passdetail", pass.name.c_str());
		if (ImGui::CollapsingHeader(headerBuf, ImGuiTreeNodeFlags_DefaultOpen)) {

			ImGui::TextColored(UIStyle::kTextDim, "Type: %s  |  Enabled: %s",
				pass.type == PassType::Compute ? "Compute" : "Graphics",
				pass.enabled ? "Yes" : "No");

			// Inputs
			if (!pass.readImages.empty() || !pass.readBuffers.empty()) {
				ImGui::SeparatorText("Inputs");
				for (auto h : pass.readImages) DrawImageRef(snap, h);
				for (auto h : pass.readBuffers) DrawBufferRef(snap, h);
			}

			// Outputs
			if (!pass.writeImages.empty() || !pass.writeBuffers.empty()) {
				ImGui::SeparatorText("Outputs");
				for (auto h : pass.writeImages) DrawImageRef(snap, h);
				for (auto h : pass.writeBuffers) DrawBufferRef(snap, h);
			}

			// Graphics-specific
			if (pass.gfx) {
				const auto& gfx = *pass.gfx;
				ImGui::SeparatorText("Attachments");
				for (size_t ci = 0; ci < gfx.colorAttachments.size(); ci++) {
					const auto& ca = gfx.colorAttachments[ci];
					ImGui::BulletText("Color[%zu]: %s  (Load: %s, Store: %s)",
						ci, ImageName(snap, ca.target),
						VkStr::LoadOpStr(ca.load), VkStr::StoreOpStr(ca.store));
				}
				if (gfx.hasDepth) {
					ImGui::BulletText("Depth: %s  (Load: %s, Store: %s)",
						ImageName(snap, gfx.depthTarget),
						VkStr::LoadOpStr(gfx.depthLoad), VkStr::StoreOpStr(gfx.depthStore));
				}
				if (gfx.hasResolve) {
					ImGui::BulletText("Resolve: %s", ImageName(snap, gfx.resolveTarget));
				}
			}

			// Barriers
			const auto& barriers = snap.barriers[m_selectedPass];
			if (!barriers.imageBarriers.empty() || !barriers.bufferBarriers.empty()) {
				ImGui::SeparatorText("Barriers");
				for (const auto& b : barriers.imageBarriers) {
					ImGui::TextColored(UIStyle::kTextDim,
						"  %s: %s -> %s  (%s -> %s)",
						ImageName(snap, b.image),
						VkStr::ImageLayout(b.oldLayout),
						VkStr::ImageLayout(b.newLayout),
						VkStr::PipelineStage(b.srcStage).c_str(),
						VkStr::PipelineStage(b.dstStage).c_str());
				}
				for (const auto& b : barriers.bufferBarriers) {
					ImGui::TextColored(UIStyle::kTextDim,
						"  %s: %s -> %s",
						BufferName(snap, b.buffer),
						VkStr::PipelineStage(b.srcStage).c_str(),
						VkStr::PipelineStage(b.dstStage).c_str());
				}
			}
		}
	}

	// ---- Resource Table ----
	if (ImGui::CollapsingHeader("Resources")) {
		// Images
		if (!snap.images.empty()) {
			ImGui::SeparatorText("Images");
			ImGuiTableFlags resTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
				| ImGuiTableFlags_SizingStretchProp;
			if (ImGui::BeginTable("##images", 6, resTableFlags)) {
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Format",   ImGuiTableColumnFlags_WidthFixed, 120.0f);
				ImGui::TableSetupColumn("Samples",  ImGuiTableColumnFlags_WidthFixed, 40.0f);
				ImGui::TableSetupColumn("Source",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableSetupColumn("Usage",    ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableHeadersRow();

				for (const auto& img : snap.images) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::Text("%s", img.name.c_str());
					ImGui::TableSetColumnIndex(1); ImGui::Text("%ux%u", img.desc.width, img.desc.height);
					ImGui::TableSetColumnIndex(2); ImGui::Text("%s", VkStr::Format(img.desc.format));
					ImGui::TableSetColumnIndex(3); ImGui::Text("%s", VkStr::SampleCount(img.desc.samples));
					ImGui::TableSetColumnIndex(4);
					ImGui::TextColored(img.imported ? UIStyle::kAccent : UIStyle::kTextDim,
						img.imported ? "Import" : "Transient");
					ImGui::TableSetColumnIndex(5);
					auto usage = VkStr::ImageUsage(img.usageFlags);
					ImGui::TextColored(UIStyle::kTextDim, "%s", usage.c_str());
				}
				ImGui::EndTable();
			}
		}

		// Buffers
		if (!snap.buffers.empty()) {
			ImGui::SeparatorText("Buffers");
			if (ImGui::BeginTable("##buffers", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableHeadersRow();

				for (const auto& buf : snap.buffers) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::Text("%s", buf.name.c_str());
					ImGui::TableSetColumnIndex(1); ImGui::Text("%llu B", (unsigned long long)buf.desc.size);
					ImGui::TableSetColumnIndex(2);
					ImGui::TextColored(buf.imported ? UIStyle::kAccent : UIStyle::kTextDim,
						buf.imported ? "Import" : "Transient");
				}
				ImGui::EndTable();
			}
		}
	}

	ImGui::End();
}
