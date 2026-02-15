#pragma once
#include "Swapchain.h"
#include "CommandBuffer.h"
#include "CommandPool.h"
#include "Image.h"
#include "ImageView.h"
#include "Semaphore.h"
#include "Fence.h"
#include "Device.h"
#include "Surface.h"
#include <functional>

namespace VWrap {

	/// <summary>
	/// Submits recorded command buffers to the graphics queue for rendering and presents their results to the surface.
	/// Maintains the swapchain and its associated resources. Controls synchronization for the rendering and presentation of frames.
	/// </summary>
	class FrameController {
	private:

		// CLASS PROPERTIES ----------------------------------------------------------------------------------
		/// <summary>
		/// The maximum number of frames that can be in flight at once.
		/// </summary>
		uint32_t frames;

		/// <summary>
		/// Whether the swapchain needs to be resized.
		/// </summary>
		bool resized = false;

		/// <summary>
		/// The frame to be recorded to and rendered.
		/// </summary>
		uint32_t m_current_frame = 0;

		/// <summary>
		/// The index of the image to be rendered to.
		/// </summary>
		uint32_t m_image_index = 0;

		/// <summary>
		/// Callback to trigger resizing of the application after swapchain resizing.
		/// </summary>
		std::function<void()> m_resize_callback;

		// RESOURCES -----------------------------------------------------------------------------------------
		/// <summary>
		/// The device used.
		/// <\summary>
		std::shared_ptr<Device> m_device;

		/// <summary>
		/// The swapchain used.
		/// </summary>
		std::shared_ptr<Swapchain> m_swapchain;

		/// <summary>
		/// The surface used to create the swapchain.
		/// </summary>
		std::shared_ptr<Surface> m_surface;

		/// <summary>
		/// The command buffers used to render frames, one for each frame.
		/// </summary>
		std::vector<std::shared_ptr<CommandBuffer>> m_command_buffers;

		/// <summary>
		/// The image views for the swapchain images.
		/// </summary>
		std::vector<std::shared_ptr<ImageView>> m_image_views;

		/// <summary>
		/// The semaphores (per frame) used to signal when an image is available for rendering, and when rendering is finished.
		/// </summary>
		std::vector<std::shared_ptr<Semaphore>> m_image_available_semaphores, m_render_finished_semaphores;

		/// <summary>
		/// The fences (per frame) used to synchronize the CPU and GPU.
		/// </summary>
		std::vector<std::shared_ptr<Fence>> m_in_flight_fences;

		/// <summary>
		/// The graphics command pool used to allocate command buffers.
		/// </summary>
		std::shared_ptr<CommandPool> m_graphics_command_pool;

		/// <summary>
		/// The present queue used to present images to the surface.
		/// </summary>
		std::shared_ptr<Queue> m_present_queue;

		// CLASS FUNCTIONS -----------------------------------------------------------------------------------
		/// <summary>
		/// Creates an image view for each swapchain image.
		/// </summary>
		void CreateImageViews();

		/// <summary>
		/// Creates the command buffers used to render frames.
		/// </summary>
		void CreateCommandBuffers();

		/// <summary>
		/// Creates the sync objects that control frames-in-flight.
		/// </summary>
		void CreateSyncObjects();

	public:

		/// <summary>
		/// Creates a new FrameController
		/// </summary>
		/// <param name="device"> The device to create all resources from </param>
		/// <param name="surface"> The surface that the swapchain is created from </param>
		/// <param name="graphics_pool"> The pool that rendering command buffers are allocated from </param>
		/// <param name="present_queue"> The queue to present rendered images </param>
		/// <param name="max_frames"> The maximumum number of frames-in-flight </param>
		/// <returns> A pointer to a new FrameController </returns>
		static std::shared_ptr<FrameController> Create(std::shared_ptr<Device> device, std::shared_ptr<Surface> surface, std::shared_ptr<CommandPool> graphics_pool, std::shared_ptr<Queue> present_queue, uint32_t max_frames);

		/// <summary>
		/// Gets the swapchain image views.
		/// </summary>
		std::vector<std::shared_ptr<ImageView>> GetImageViews() { return m_image_views; }

		/// <summary>
		/// Gets the swapchain.
		/// </summary>
		std::shared_ptr<Swapchain> GetSwapchain() { return m_swapchain; }

		/// <summary>
		/// Gets the current frame to be rendered.
		/// </summary>
		uint32_t GetCurrentFrame() { return m_current_frame; }

		/// <summary>
		/// Gets the index of the image to be rendered to.
		/// </summary>
		uint32_t GetImageIndex() { return m_image_index; }

		/// <summary>
		/// Gets the command buffer for the current frame.
		/// </summary>
		std::shared_ptr<CommandBuffer> GetCurrentCommandBuffer() { return m_command_buffers[m_current_frame]; }

		/// <summary>
		/// Waits for the GPU to finish rendering the current frame. Then it acquires the next image to be rendered to.
		/// </summary>
		void AcquireNext();

		/// <summary>
		/// Submits the current frame to the graphics queue for rendering and presents the image to the surface.
		/// </summary>
		void Render();

		/// <summary>
		/// Recreates the swapchain and all associated resources.
		/// </summary>
		void RecreateSwapchain();

		/// <summary>
		/// Sets the callback to be triggered after swapchain resizing.
		/// </summary>
		void SetResizeCallback(std::function<void()> lambda) { m_resize_callback = lambda; }

		/// <summary>
		/// Sets whether the swapchain needs to be resized.
		/// </summary>
		void SetResized(bool resized) { this->resized = resized; }
	};
}