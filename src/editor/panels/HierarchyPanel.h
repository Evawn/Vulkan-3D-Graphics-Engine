#pragma once

#include "imgui.h"

#include <functional>

class Scene;
class SceneNode;

// Panel that shows the scene tree as a collapsible ImGui hierarchy. Selection
// state is owned by the panel; the inspector receives the selected SceneNode*
// via SetSelectionChangedCallback (or by polling GetSelected()).
//
// Stays scene-agnostic — takes a Scene* on Draw() so swapping scenes (future
// foliage editor mode) is just a parameter change.
class HierarchyPanel {
public:
	void SetSelectionChangedCallback(std::function<void(SceneNode*)> cb) {
		m_onSelectionChanged = std::move(cb);
	}

	SceneNode* GetSelected() const { return m_selected; }
	void       ClearSelection();

	// Draw the panel for the given scene. Pass nullptr to render an empty
	// "(no scene)" hint without crashing.
	void Draw(Scene* scene);

private:
	void DrawNode(SceneNode& node, int depthIndex);

	SceneNode* m_selected = nullptr;
	std::function<void(SceneNode*)> m_onSelectionChanged;
};
