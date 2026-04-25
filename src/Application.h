#pragma once

#include "imgui.h"

// PROJECT INCLUDES ---------------------------------------------------------------------------------------------
#include "Window.h"
#include "VulkanContext.h"
#include "CameraController.h"
#include "Editor.h"
#include "Camera.h"
#include "RenderingSystem.h"

// STD INCLUDES ----------------------------------------------------------------------------------------------
#include <memory>
#include <chrono>
#include <spdlog/spdlog.h>

// CONSTANTS -------------------------------------------------------------------------------------------------

const uint32_t MAX_FRAMES_IN_FLIGHT = 2;

#ifdef NDEBUG
const bool ENABLE_VALIDATION_LAYERS = false;
#else
const bool ENABLE_VALIDATION_LAYERS = true;
#endif

// APPLICATION STATE -----------------------------------------------------------------------------------------

enum class AppState { Initializing, Running, ShuttingDown };

class Application {

private:

	AppState m_state = AppState::Initializing;

	// WINDOW
	std::unique_ptr<Window> m_window;

	// VULKAN CONTEXT
	VWrap::VulkanContext m_vk;

	// RENDER PASS (kept for ImGui init compatibility)
	std::shared_ptr<VWrap::RenderPass> m_presentation_render_pass;

	// EDITOR (UI + panels)
	Editor m_editor;

	// CAMERA
	std::shared_ptr<Camera> m_camera;
	std::shared_ptr<CameraController> m_camera_controller;

	// RENDERING SYSTEM (owns Renderer, technique list, event queue, profiler)
	RenderingSystem m_rendering;

	// Per-frame metrics snapshot — Editor stores a pointer, so we own the slot.
	GPUProfiler::PerformanceMetrics m_metrics_snapshot{};

public:
	void Run();

private:
	void Init();
	void InitVulkan();
	void MainLoop();
	void Cleanup();
	void DrawFrame();
};
