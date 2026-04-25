#pragma once

#include <cstddef>

// Rendering-related events posted by techniques, panels, and other UI hooks.
// Owned and drained by RenderingSystem::ProcessEvents. (DPI changes don't go
// through this queue — they're a pure UI concern handled inline by Application.)
enum class AppEventType {
	HotReloadShaders,
	SwitchRenderer,
	CaptureScreenshot,
	ReloadTechnique,
	RebuildGraph,
	RecreatePipelines,
};

struct AppEvent {
	AppEventType type;
	size_t index = 0;      // SwitchRenderer
};
