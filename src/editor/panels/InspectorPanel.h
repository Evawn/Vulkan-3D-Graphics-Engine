#pragma once

#include "imgui.h"
#include "RenderTechnique.h"
#include "SceneLighting.h"
#include "SkyDescription.h"
#include "Camera.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>

class PostProcessChain;
class SceneNode;
namespace Capture { class CaptureSystem; }

class InspectorPanel {
private:
	// Technique
	std::vector<std::unique_ptr<RenderTechnique>>* m_renderers = nullptr;
	size_t* m_active_index = nullptr;
	std::function<void()> m_reload_callback;
	std::function<void(size_t)> m_switch_callback;

	// Camera
	std::shared_ptr<Camera> m_camera;
	float* m_sensitivity = nullptr;
	float* m_speed = nullptr;

	// Capture (screenshots + MP4 recording). Live status comes from the
	// CaptureSystem pointer; the toggle callback fans out to the rendering
	// event queue so the actual ToggleRecording runs at the start of the
	// next frame's drain (consistent with screenshot dispatch).
	std::function<void()>   m_screenshot_callback;
	std::function<void()>   m_toggle_recording_callback;
	std::string             m_last_screenshot_path;
	std::string             m_last_recording_path;
	Capture::CaptureSystem* m_capture = nullptr;

	// Lighting + sky + post-processing (Scene-owned; panel just edits them)
	SceneLighting*    m_lighting = nullptr;
	SkyDescription*   m_sky      = nullptr;
	PostProcessChain* m_post_process = nullptr;

public:
	void SetRenderers(std::vector<std::unique_ptr<RenderTechnique>>* renderers, size_t* activeIndex);
	void SetReloadCallback(std::function<void()> cb) { m_reload_callback = std::move(cb); }
	void SetSwitchCallback(std::function<void(size_t)> cb) { m_switch_callback = std::move(cb); }
	void SetCamera(std::shared_ptr<Camera> camera) { m_camera = camera; }
	void SetAppControls(float* sensitivity, float* speed) { m_sensitivity = sensitivity; m_speed = speed; }
	void SetScreenshotCallback(std::function<void()> cb) { m_screenshot_callback = std::move(cb); }
	void SetToggleRecordingCallback(std::function<void()> cb) { m_toggle_recording_callback = std::move(cb); }
	void SetLastScreenshotPath(const std::string& path) { m_last_screenshot_path = path; }
	void SetLastRecordingPath (const std::string& path) { m_last_recording_path  = path; }
	void SetCaptureSystem(Capture::CaptureSystem* capture) { m_capture = capture; }
	void SetLighting(SceneLighting* lighting) { m_lighting = lighting; }
	void SetSky(SkyDescription* sky)          { m_sky = sky; }
	void SetPostProcess(PostProcessChain* chain) { m_post_process = chain; }

	// Drives the "Selected Node" section. Driven by HierarchyPanel selection
	// changes; nullptr hides the section.
	void SetSelectedNode(SceneNode* node) { m_selected_node = node; }

	// External invokers (menu bar / hotkeys) re-use the inspector-registered
	// callbacks so the engine has one event channel for these actions.
	void RequestReload() { if (m_reload_callback) m_reload_callback(); }
	void RequestSwitchTechnique(size_t idx) { if (m_switch_callback) m_switch_callback(idx); }
	void RequestScreenshot() { if (m_screenshot_callback) m_screenshot_callback(); }
	void RequestToggleRecording() { if (m_toggle_recording_callback) m_toggle_recording_callback(); }

	void Draw();

private:
	SceneNode* m_selected_node = nullptr;

	// Latched on first Draw() so the Renderer tab is the default-active tab on
	// startup. Passing ImGuiTabItemFlags_SetSelected every frame would lock the
	// tab and prevent the user from switching, so the flag fires exactly once.
	bool m_default_tab_applied = false;
};
