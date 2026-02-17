#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

// PROJECT INCLUDES ---------------------------------------------------------------------------------------------
#include "VulkanContext.h"
#include "CameraController.h"
#include "GUIRenderer.h"
#include "GPUProfiler.h"
#include <optional>
#include "Camera.h"
#include "RenderTechnique.h"
#include "Renderer.h"
#include "DDATracer.h"
#include "MeshRasterizer.h"
#include "ComputeTest.h"
#include "Sampler.h"

// PANELS
#include "ViewportPanel.h"
#include "MetricsPanel.h"
#include "OutputPanel.h"
#include "InspectorPanel.h"

// STD INCLUDES ----------------------------------------------------------------------------------------------
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <vector>
#include <array>
#include <chrono>
#include <spdlog/spdlog.h>

// CONSTANTS -------------------------------------------------------------------------------------------------

const uint32_t MAX_FRAMES_IN_FLIGHT = 2;

#ifdef NDEBUG
const bool ENABLE_VALIDATION_LAYERS = false;
#else
const bool ENABLE_VALIDATION_LAYERS = true;
#endif

static void check_vk_result(VkResult err)
{
	if (err == 0)
		return;
	spdlog::get("App")->error("VkResult = {}", (int)err);
	if (err < 0)
		abort();
}

class Application {

private:

	// VULKAN CONTEXT
	VWrap::VulkanContext m_vk;

	// WINDOW
	std::shared_ptr<GLFWwindow*> m_glfw_window;

	// RENDER PASS (kept for ImGui init compatibility)
	std::shared_ptr<VWrap::RenderPass> m_presentation_render_pass;

	// RENDERER
	Renderer m_renderer;
	VkExtent2D m_offscreen_extent{};

	// SCENE TEXTURE (for ImGui viewport)
	std::shared_ptr<VWrap::Sampler> m_scene_sampler;
	VkDescriptorSet m_scene_texture = VK_NULL_HANDLE;

	// GUI
	std::shared_ptr<GUIRenderer> m_gui_renderer;

	// PANELS
	ViewportPanel m_viewport_panel;
	MetricsPanel m_metrics_panel;
	OutputPanel m_output_panel;
	InspectorPanel m_inspector_panel;

	// GPU PROFILER
	std::shared_ptr<GPUProfiler> m_gpu_profiler;
	GPUProfiler::PerformanceMetrics m_last_metrics{0.0f, 0.0f};

	// CAMERA
	std::shared_ptr<Camera> m_camera;
	std::shared_ptr<CameraController> m_camera_controller;

	// RENDERERS
	std::vector<std::unique_ptr<RenderTechnique>> m_renderers;
	size_t m_active_renderer_index = 0;
	bool m_pending_hot_reload = false;
	std::optional<size_t> m_pending_renderer_switch;
	bool m_pending_dpi_update = false;
	float m_pending_dpi_scale = 1.0f;

public:
	void Run();

private:
	static void glfw_FramebufferResizeCallback(GLFWwindow* window, int width, int height);
	static void glfw_WindowFocusCallback(GLFWwindow* window, int focused);
	static void glfw_ContentScaleCallback(GLFWwindow* window, float xscale, float yscale);
	static GLFWmonitor* GetCurrentMonitor(GLFWwindow* window);

	void Init();
	void InitWindow();
	void InitVulkan();
	void InitImGui();
	void InitPanels();
	void MainLoop();
	void Cleanup();
	void Resize();
	void BuildRenderGraph();
	void DrawFrame();
	void HotReloadShaders();
	void SwitchRenderer(size_t index);
	void CaptureScreenshot();
	RenderContext BuildRenderContext() const;
	void RegisterSceneTexture();
};
