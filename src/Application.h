#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "tiny_obj_loader.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

// PROJECT INCLUDES ---------------------------------------------------------------------------------------------
#include "VulkanContext.h"
#include "OffscreenTarget.h"
#include "GUIRenderer.h"
#include "GPUProfiler.h"
#include "Camera.h"
#include "Input.h"
#include "RenderTechnique.h"
#include "OctreeTracer.h"
#include "MeshRasterizer.h"

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

const uint32_t WIDTH = 1200;
const uint32_t HEIGHT = 900;
const uint32_t MAX_FRAMES_IN_FLIGHT = 2;

#ifdef NDEBUG
const bool ENABLE_VALIDATION_LAYERS = false;
#else
const bool ENABLE_VALIDATION_LAYERS = true;
#endif

enum class Action {
	ESCAPE, MOVE_FORWARD, MOVE_BACKWARD, MOVE_LEFT, MOVE_RIGHT, MOVE_UP, MOVE_DOWN,
	RELOAD_SHADERS
};

static void check_vk_result(VkResult err)
{
	if (err == 0)
		return;
	spdlog::get("App")->error("VkResult = {}", (int)err);
	if (err < 0)
		abort();
}

struct CameraMoveState {
	bool up, down, left, right, forward, back = false;
	double dx, dy = 0.0;
};

static CameraMoveState move_state{ false, false, false, false, false, false };

class Application {

private:

	// VULKAN CONTEXT
	VulkanContext m_vk;

	// WINDOW
	std::shared_ptr<GLFWwindow*> m_glfw_window;

	// RENDER PASSES
	std::shared_ptr<VWrap::RenderPass> m_scene_render_pass;
	std::shared_ptr<VWrap::RenderPass> m_presentation_render_pass;

	// PRESENTATION FRAMEBUFFERS (one per swapchain image)
	std::vector<std::shared_ptr<VWrap::Framebuffer>> m_presentation_framebuffers;

	// OFFSCREEN TARGET (scene renders here)
	std::shared_ptr<OffscreenTarget> m_offscreen_target;

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

	// RENDERERS
	std::vector<std::unique_ptr<RenderTechnique>> m_renderers;
	size_t m_active_renderer_index = 0;

	// INPUT
	Context m_main_context = {
		"Main",
		{
			{{GLFW_KEY_ESCAPE, KeyState::PRESSED}, (int)Action::ESCAPE},
			{{GLFW_KEY_W, KeyState::DOWN}, (int)Action::MOVE_FORWARD},
			{{GLFW_KEY_S, KeyState::DOWN}, (int)Action::MOVE_BACKWARD},
			{{GLFW_KEY_A, KeyState::DOWN}, (int)Action::MOVE_LEFT},
			{{GLFW_KEY_D, KeyState::DOWN}, (int)Action::MOVE_RIGHT},
			{{GLFW_KEY_SPACE, KeyState::DOWN}, (int)Action::MOVE_UP},
			{{GLFW_KEY_LEFT_SHIFT, KeyState::DOWN}, (int)Action::MOVE_DOWN},
			{{GLFW_KEY_F5, KeyState::PRESSED}, (int)Action::RELOAD_SHADERS}
		}
	};

	struct AppState {
		bool focused = false;
		float sensitivity = 0.5f;
		float speed = 5.0f;
	};
	AppState m_app_state;

public:
	void Run();

private:
	static void glfw_FramebufferResizeCallback(GLFWwindow* window, int width, int height);
	static void glfw_WindowFocusCallback(GLFWwindow* window, int focused);

	void MoveCamera(float dt);

	void Init();
	void InitWindow();
	void InitVulkan();
	void InitImGui();
	void InitPanels();
	void MainLoop();
	void ParseInputQuery(InputQuery actions);
	void Cleanup();
	void Resize();
	void CreatePresentationFramebuffers();
	void DrawFrame();
	void HotReloadShaders();
	void SwitchRenderer(size_t index);
	void CaptureScreenshot();
};
