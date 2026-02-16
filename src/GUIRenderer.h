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

class GUIRenderer
{
private:
	std::shared_ptr<VWrap::DescriptorPool> m_imgui_descriptor_pool;
	float m_dpi_scale = 1.0f;
	bool m_first_frame = true;

	struct Panel {
		std::string name;
		std::function<void()> drawFn;
	};
	std::vector<Panel> m_panels;

	void SetupDefaultLayout(ImGuiID dockspace_id);

public:
	static std::shared_ptr<GUIRenderer> Create(std::shared_ptr<VWrap::Device> device);

	void RegisterPanel(const std::string& name, std::function<void()> drawFn);
	void CmdDraw(std::shared_ptr<VWrap::CommandBuffer> command_buffer);

	void BeginFrame();

	std::shared_ptr<VWrap::DescriptorPool> GetDescriptorPool() const {
		return m_imgui_descriptor_pool;
	}

	void SetDpiScale(float dpi_scale) {
		ImGuiIO& io = ImGui::GetIO();
		m_dpi_scale = dpi_scale;
		io.FontGlobalScale = dpi_scale;
	}
};
