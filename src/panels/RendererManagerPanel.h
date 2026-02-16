#pragma once

#include "imgui.h"
#include "RenderTechnique.h"
#include <vector>
#include <memory>
#include <functional>

class RendererManagerPanel {
private:
	std::vector<std::unique_ptr<RenderTechnique>>* m_renderers = nullptr;
	size_t* m_active_index = nullptr;
	std::function<void()> m_reload_callback;
	std::function<void(size_t)> m_switch_callback;

	float* m_sensitivity = nullptr;
	float* m_speed = nullptr;

public:
	void SetRenderers(std::vector<std::unique_ptr<RenderTechnique>>* renderers, size_t* activeIndex);
	void SetReloadCallback(std::function<void()> cb) { m_reload_callback = std::move(cb); }
	void SetSwitchCallback(std::function<void(size_t)> cb) { m_switch_callback = std::move(cb); }
	void SetAppControls(float* sensitivity, float* speed) { m_sensitivity = sensitivity; m_speed = speed; }
	void Draw();
};
