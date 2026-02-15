#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "tiny_obj_loader.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

// VWRAP INCLUDES ---------------------------------------------------------------------------------------------
#include "Instance.h"
#include "Surface.h"
#include "PhysicalDevice.h"
#include "Device.h"
#include "Swapchain.h"
#include "ImageView.h"
#include "Image.h"
#include "RenderPass.h"
#include "DescriptorSetLayout.h"
#include "Framebuffer.h"
#include "Pipeline.h"
#include "CommandPool.h"
#include "CommandBuffer.h"
#include "Fence.h"
#include "Semaphore.h"
#include "Sampler.h"
#include "Buffer.h"
#include "DescriptorPool.h"
#include "DescriptorSet.h"
#include "Queue.h"
#include "FrameController.h"
#include "Allocator.h"

// PROJECT INCLUDES ---------------------------------------------------------------------------------------------
#include "GUIRenderer.h"
#include "GPUProfiler.h"
#include "Camera.h"
#include "Input.h"
#include "OctreeTracer.h"

// STD INCLUDES ----------------------------------------------------------------------------------------------
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <vector>
#include <unordered_set>
#include <optional>
#include <set>
#include <algorithm>
#include <fstream>
#include <array>
#include <chrono>

// CONSTANTS -------------------------------------------------------------------------------------------------

/// <summary>
/// The width and height of the viewport. (In screen coordinates.)
/// </summary>
const uint32_t WIDTH = 1200;
const uint32_t HEIGHT = 900;

/// <summary>
/// The maximum number of frames that can be in flight at once.
/// </summary>
const uint32_t MAX_FRAMES_IN_FLIGHT = 2;

/// <summary>
/// Whether or not to enable validation layers. (Debugging only.)
/// </summary>
#ifdef NDEBUG
const bool ENABLE_VALIDATION_LAYERS = false;
#else
const bool ENABLE_VALIDATION_LAYERS = true;
#endif

enum class Action {
	ESCAPE, MOVE_FORWARD, MOVE_BACKWARD, MOVE_LEFT, MOVE_RIGHT, MOVE_UP, MOVE_DOWN
};



static void check_vk_result(VkResult err)
{
	if (err == 0)
		return;
	fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
	if (err < 0)
		abort();
}

struct CameraMoveState {
	bool up, down, left, right, forward, back = false;
	double dx, dy = 0.0;
};

static CameraMoveState move_state{ false, false, false, false, false, false };

/// <summary>
/// This class is the main application class. It contains all the vulkan objects
/// and the main loop. 
/// </summary>
class Application {

private:

	// VWRAP OBJECTS ---------------------------------------------------------------------------------------------
	// VULKAN BASE
	std::shared_ptr<VWrap::Instance> m_instance;
	std::shared_ptr<VWrap::PhysicalDevice> m_physical_device;
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;


	// WINDOW PRESENTATION
	std::shared_ptr<GLFWwindow*> m_glfw_window;
	std::shared_ptr<VWrap::Surface> m_surface;
	std::shared_ptr<VWrap::FrameController> m_frame_controller;

	// COMMANDS
	std::shared_ptr<VWrap::CommandPool> m_graphics_command_pool;
	std::shared_ptr<VWrap::CommandPool> m_transfer_command_pool;
	std::shared_ptr<VWrap::Queue> m_graphics_queue;
	std::shared_ptr<VWrap::Queue> m_present_queue;
	std::shared_ptr<VWrap::Queue> m_transfer_queue;

	// RENDER PASS
	std::shared_ptr<VWrap::RenderPass> m_render_pass;
	std::vector<std::shared_ptr<VWrap::Framebuffer>> m_framebuffers;
	std::shared_ptr<VWrap::ImageView> m_depth_image_view;
	std::shared_ptr<VWrap::ImageView> m_color_image_view;

	/// <summary>
	/// Contains and manages the resources needed to render GUI elements.
	/// </summary>
	std::shared_ptr<GUIRenderer> m_gui_renderer;

	/// <summary>
	/// Contains and manages the resources needed to profile the GPU.
	/// </summary>
	std::shared_ptr<GPUProfiler> m_gpu_profiler;

	/// <summary>
	/// The camera used to view the scene.
	/// </summary>
	std::shared_ptr<Camera> m_camera;

	std::shared_ptr<OctreeTracer> m_octree_tracer;

	Context m_main_context = {
		"Main",
		{
			{{GLFW_KEY_ESCAPE, KeyState::PRESSED}, (int)Action::ESCAPE},
			{{GLFW_KEY_W, KeyState::DOWN}, (int)Action::MOVE_FORWARD},
			{{GLFW_KEY_S, KeyState::DOWN}, (int)Action::MOVE_BACKWARD},
			{{GLFW_KEY_A, KeyState::DOWN}, (int)Action::MOVE_LEFT},
			{{GLFW_KEY_D, KeyState::DOWN}, (int)Action::MOVE_RIGHT},
			{{GLFW_KEY_SPACE, KeyState::DOWN}, (int)Action::MOVE_UP},
			{{GLFW_KEY_LEFT_SHIFT, KeyState::DOWN}, (int)Action::MOVE_DOWN}
		}
	};

	struct AppState {
		bool focused = true;
		float sensitivity = 0.5f;
		float speed = 5.0f;
	};
	AppState m_app_state;

	//std::shared_ptr<Input<Action>> m_input;

	// CLASS FUNCTIONS -------------------------------------------------------------------------------------------
public:

	/// <summary>
	/// Entrypoint to the application. Creates all resources and runs the main loop. Handles cleanup.
	/// </summary>
	void Run();

private:
	/// <summary>
	/// Callback function for when the window is resized. Notifies the frame controller to resize.
	/// </summary>
	static void glfw_FramebufferResizeCallback(GLFWwindow* window, int width, int height);

	static void glfw_WindowFocusCallback(GLFWwindow* window, int focused);

	void MoveCamera(float dt);

	void Init();

	/// <summary>
	/// Initializes the window and sets references in GLFW to the app and resize callback.
	/// </summary>
	void InitWindow();

	/// <summary>
	/// Initializes the vulkan objects.
	/// </summary>
	void InitVulkan();

	/// <summary>
	/// Initializes ImGUI.
	/// </summary>
	void InitImGui();

	/// <summary>
	/// Contains the main loop of the application, which polls for events and draws frames.
	/// </summary>
	void MainLoop();

	void ParseInputQuery(InputQuery actions);

	/// <summary>
	/// Cleans up any resources that need to be explicitly destroyed.
	/// </summary>
	void Cleanup();

	/// <summary>
	/// Called by the frame controller when the window is resized. Resizes all necessary resources.
	/// </summary>
	void Resize();

	/// <summary>
	/// Creates a framebuffer for each swapchain image.
	/// </summary>
	void CreateFramebuffers();

	/// <summary>
	/// Creates the depth image and image view.
	/// </summary>
	void CreateDepthResources(VkSampleCountFlagBits samples);

	/// <summary>
	/// Creates the color image and image view.
	/// </summary>
	void CreateColorResources(VkSampleCountFlagBits samples);

	/// <summary>
	/// Performs the main rendering operations.
	/// </summary>
	void DrawFrame();
};