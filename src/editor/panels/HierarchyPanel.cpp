#include "HierarchyPanel.h"
#include "Scene.h"
#include "UIStyle.h"

void HierarchyPanel::ClearSelection() {
	if (m_selected) {
		m_selected = nullptr;
		if (m_onSelectionChanged) m_onSelectionChanged(nullptr);
	}
}

void HierarchyPanel::Draw(Scene* scene) {
	ImGui::Begin("Hierarchy");

	if (!scene) {
		ImGui::TextColored(UIStyle::kTextDim, "(no scene)");
		ImGui::End();
		return;
	}

	// Validate the cached selection. The scene tree owns nodes via unique_ptr,
	// and we hold a non-owning pointer; if the selected node was removed (not
	// possible in v1 but cheap to guard), we clear the selection. Today the
	// tree only grows, so this is a safety net for future scene-edit features.
	// For now we just trust the pointer.

	DrawNode(scene->GetRoot(), 0);

	ImGui::End();
}

void HierarchyPanel::DrawNode(SceneNode& node, int depthIndex) {
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
		| ImGuiTreeNodeFlags_OpenOnDoubleClick
		| ImGuiTreeNodeFlags_SpanAvailWidth;

	if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
	if (depthIndex == 0)       flags |= ImGuiTreeNodeFlags_DefaultOpen;
	if (m_selected == &node)   flags |= ImGuiTreeNodeFlags_Selected;

	const std::string& displayName = node.name.empty() ? std::string("(unnamed)") : node.name;

	// Use the node pointer as the ImGui ID so siblings with the same name
	// don't collide.
	ImGui::PushID(static_cast<const void*>(&node));
	bool open = ImGui::TreeNodeEx(displayName.c_str(), flags);

	// Component badge: show how many components the node carries, dimmed.
	if (!node.components.empty()) {
		ImGui::SameLine();
		ImGui::TextColored(UIStyle::kTextDim, "[%zu]", node.components.size());
	}

	// Selection: click on the row (not on the arrow) selects.
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
		m_selected = &node;
		if (m_onSelectionChanged) m_onSelectionChanged(m_selected);
	}

	if (open) {
		for (auto& child : node.children) {
			DrawNode(*child, depthIndex + 1);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}
