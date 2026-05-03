#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include "DescriptorPool.h"
#include "RenderPass.h"
#include "Device.h"
#include "Queue.h"
#include "CommandBuffer.h"
#include "UIState.h"

class GUIRenderer
{
private:
	std::shared_ptr<VWrap::DescriptorPool> m_imgui_descriptor_pool;
	float m_dpi_scale = 1.0f;
	bool m_first_frame = true;
	UIState* m_ui = nullptr;

	// Dock IDs captured during SetupDefaultLayout so we can re-apply absolute sizes
	// on window resize without rebuilding the whole layout (which would clobber user drags).
	ImGuiID m_dock_left = 0;
	ImGuiID m_dock_right = 0;
	ImGuiID m_dock_left_top = 0;
	ImGuiID m_dock_left_bottom = 0;
	ImGuiID m_dock_right_top = 0;
	ImGuiID m_dock_right_bottom = 0;
	bool m_last_viewport_only = false;
	LayoutPreset m_last_preset = LayoutPreset::Develop;

	struct Panel {
		std::string name;
		std::function<void()> drawFn;
		bool visible = true;     // workspaces gate this; defaults visible
	};
	std::vector<Panel> m_panels;

	// Optional menu bar / status bar callbacks installed by Editor. Drawn outside
	// the dockspace so they always sit at the very top / bottom of the viewport.
	std::function<void()> m_menu_bar_fn;
	std::function<void()> m_status_bar_fn;

	void SetupDefaultLayout(ImGuiID dockspace_id);
	void BuildLayout(ImGuiID dockspace_id, LayoutPreset preset, bool viewport_only,
	                 float right_px, float bottom_px);

public:
	static std::shared_ptr<GUIRenderer> Create(std::shared_ptr<VWrap::Device> device);

	void SetUIState(UIState* ui) { m_ui = ui; }
	void RegisterPanel(const std::string& name, std::function<void()> drawFn);
	// Toggle whether a registered panel's drawFn is invoked. Workspaces use
	// this to hide panels that don't belong (e.g. Hierarchy in ImportBake).
	// No-op for unknown names — caller doesn't need to track which panels were
	// registered, only which ones to gate.
	void SetPanelVisible(const std::string& name, bool visible);
	void SetMenuBar(std::function<void()> fn)   { m_menu_bar_fn   = std::move(fn); }
	void SetStatusBar(std::function<void()> fn) { m_status_bar_fn = std::move(fn); }
	void CmdDraw(std::shared_ptr<VWrap::CommandBuffer> command_buffer);

	void BeginFrame();
	void LoadFonts(float dpi_scale);

	std::shared_ptr<VWrap::DescriptorPool> GetDescriptorPool() const {
		return m_imgui_descriptor_pool;
	}
};
