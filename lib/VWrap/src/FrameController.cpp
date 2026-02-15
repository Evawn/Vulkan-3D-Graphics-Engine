#include "FrameController.h"

namespace VWrap {
	std::shared_ptr<FrameController> FrameController::Create(std::shared_ptr<Device> device, std::shared_ptr<Surface> surface, std::shared_ptr<CommandPool> graphics_pool, std::shared_ptr<Queue> present_queue, uint32_t max_frames) {
		auto ret = std::make_shared<FrameController>();
		ret->m_device = device;
		ret->m_graphics_command_pool = graphics_pool;
		ret->m_present_queue = present_queue;
		ret->m_surface = surface;
		ret->m_swapchain = Swapchain::Create(device, surface);
		ret->frames = max_frames;
		ret->CreateImageViews();
		ret->CreateCommandBuffers();
		ret->CreateSyncObjects();

		return ret;
	}

	void FrameController::AcquireNext() {
		VkFence fences[] = { m_in_flight_fences[m_current_frame]->Get() };
		vkWaitForFences(m_device->Get(), 1, fences, VK_TRUE, UINT64_MAX);

		//uint32_t imageIndex;
		VkResult result = vkAcquireNextImageKHR(m_device->Get(), m_swapchain->Get(), UINT64_MAX, m_image_available_semaphores[m_current_frame]->Get(), VK_NULL_HANDLE, &m_image_index);

		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			RecreateSwapchain();
			return;
		}
		else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}

		vkResetCommandBuffer(m_command_buffers[m_current_frame]->Get(), 0);
	}

	void FrameController::Render() {
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		VkSemaphore waitSemaphores[] = { m_image_available_semaphores[m_current_frame]->Get() };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = 1;

		std::array<VkCommandBuffer, 1> commandBuffers = { m_command_buffers[m_current_frame]->Get() };
		submitInfo.pCommandBuffers = commandBuffers.data();

		VkSemaphore signalSemaphores[] = { m_render_finished_semaphores[m_current_frame]->Get() };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		VkFence fences[] = { m_in_flight_fences[m_current_frame]->Get() };
		vkResetFences(m_device->Get(), 1, fences);

		if (vkQueueSubmit(m_graphics_command_pool->GetQueue()->Get(),
			1,
			&submitInfo,
			m_in_flight_fences[m_current_frame]->Get()) != VK_SUCCESS) {

			throw std::runtime_error("Failed to submit to graphics queue!");
		}

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapchains[] = { m_swapchain->Get() };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapchains;
		presentInfo.pImageIndices = &m_image_index;
		presentInfo.pResults = nullptr; // Optional

		VkResult result = vkQueuePresentKHR(m_present_queue->Get(), &presentInfo);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || resized) {
			resized = false;
			RecreateSwapchain();
		}
		else if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}

		m_current_frame = (m_current_frame + 1) % frames;
	}

	void FrameController::RecreateSwapchain() {
		int width = 0, height = 0;

		glfwGetFramebufferSize(m_surface->GetWindow().get()[0], &width, &height);
		while (width == 0 || height == 0) {
			glfwGetFramebufferSize(m_surface->GetWindow().get()[0], &width, &height);
			glfwWaitEvents();
		}
		vkDeviceWaitIdle(m_device->Get());


		for (auto image_view : m_image_views)
			image_view.reset();
		m_swapchain.reset();

		m_swapchain = VWrap::Swapchain::Create(m_device, m_surface);
		CreateImageViews();

		if (m_resize_callback)
			m_resize_callback();
	}

	void FrameController::CreateImageViews() {
		m_image_views.resize(m_swapchain->Size());
		for (size_t i = 0; i < m_swapchain->Size(); i++)
			m_image_views[i] = VWrap::ImageView::Create(m_device, m_swapchain->GetVkImages()[i], m_swapchain->GetFormat());
	}

	void FrameController::CreateCommandBuffers() {
		m_command_buffers.resize(frames);
		for (size_t i = 0; i < frames; i++)
			m_command_buffers[i] = VWrap::CommandBuffer::Create(m_graphics_command_pool);
	}

	void FrameController::CreateSyncObjects() {
		m_image_available_semaphores.resize(frames);
		m_render_finished_semaphores.resize(frames);
		m_in_flight_fences.resize(frames);

		for (size_t i = 0; i < frames; i++) {
			m_image_available_semaphores[i] = VWrap::Semaphore::Create(m_device);
			m_render_finished_semaphores[i] = VWrap::Semaphore::Create(m_device);
			m_in_flight_fences[i] = VWrap::Fence::Create(m_device);
		}
	}
}