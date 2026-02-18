#pragma once

#include "imgui.h"

// PROJECT INCLUDES ---------------------------------------------------------------------------------------------
#include "Window.h"
#include "VulkanContext.h"
#include "CameraController.h"
#include "Editor.h"
#include "GPUProfiler.h"
#include "Camera.h"
#include "RenderTechnique.h"
#include "Renderer.h"
#include "DDATracer.h"
#include "MeshRasterizer.h"
#include "ComputeTest.h"
#include "SVORenderer.h"

// STD INCLUDES ----------------------------------------------------------------------------------------------
#include <vector>
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

enum class AppEventType { HotReloadShaders, SwitchRenderer, DpiChanged, CaptureScreenshot };

struct AppEvent {
	AppEventType type;
	size_t index = 0;      // SwitchRenderer
	float scale = 1.0f;    // DpiChanged
};

class Application {

private:

	AppState m_state = AppState::Initializing;

	// WINDOW
	std::unique_ptr<Window> m_window;

	// VULKAN CONTEXT
	VWrap::VulkanContext m_vk;

	// RENDER PASS (kept for ImGui init compatibility)
	std::shared_ptr<VWrap::RenderPass> m_presentation_render_pass;

	// RENDERER
	Renderer m_renderer;
	VkExtent2D m_offscreen_extent{};

	// EDITOR (UI + panels)
	Editor m_editor;

	// GPU PROFILER
	std::shared_ptr<GPUProfiler> m_gpu_profiler;
	GPUProfiler::PerformanceMetrics m_last_metrics{};

	// RENDER GRAPH SNAPSHOT (for dev tooling panel)
	GraphSnapshot m_graphSnapshot;

	// CAMERA
	std::shared_ptr<Camera> m_camera;
	std::shared_ptr<CameraController> m_camera_controller;

	// RENDERERS
	std::vector<std::unique_ptr<RenderTechnique>> m_renderers;
	size_t m_active_renderer_index = 0;

	// EVENT QUEUE
	std::vector<AppEvent> m_events;
	void PushEvent(AppEvent event);
	void ProcessEvents();

public:
	void Run();

private:
	void Init();
	void InitVulkan();
	void MainLoop();
	void Cleanup();
	void Resize();
	void BuildRenderGraph();
	void DrawFrame();
	void HotReloadShaders();
	void SwitchRenderer(size_t index);
	void CaptureScreenshot();
	RenderContext BuildRenderContext() const;
};
