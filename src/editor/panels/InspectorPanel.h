#pragma once

#include "imgui.h"
#include "RenderTechnique.h"
#include "SceneLighting.h"
#include "Camera.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>

class PostProcessChain;

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

	// Screenshot
	std::function<void()> m_screenshot_callback;
	std::string m_last_screenshot_path;

	// Lighting + post-processing (owned by Renderer; panel just edits them)
	SceneLighting* m_lighting = nullptr;
	PostProcessChain* m_post_process = nullptr;

public:
	void SetRenderers(std::vector<std::unique_ptr<RenderTechnique>>* renderers, size_t* activeIndex);
	void SetReloadCallback(std::function<void()> cb) { m_reload_callback = std::move(cb); }
	void SetSwitchCallback(std::function<void(size_t)> cb) { m_switch_callback = std::move(cb); }
	void SetCamera(std::shared_ptr<Camera> camera) { m_camera = camera; }
	void SetAppControls(float* sensitivity, float* speed) { m_sensitivity = sensitivity; m_speed = speed; }
	void SetScreenshotCallback(std::function<void()> cb) { m_screenshot_callback = std::move(cb); }
	void SetLastScreenshotPath(const std::string& path) { m_last_screenshot_path = path; }
	void SetLighting(SceneLighting* lighting) { m_lighting = lighting; }
	void SetPostProcess(PostProcessChain* chain) { m_post_process = chain; }
	void Draw();
};
