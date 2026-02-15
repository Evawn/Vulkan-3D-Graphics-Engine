#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <memory>
#include "PhysicalDevice.h"
#include "Device.h"
#include "Surface.h"
#include <algorithm>

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan swapchain.
	/// </summary>
	class Swapchain {

	private:

		/// <summary>
		/// The underlying Vulkan swapchain handle.
		/// </summary>
		VkSwapchainKHR m_swapchain{ VK_NULL_HANDLE };

		/// <summary> The device this swapchain was created for. </summary>
		std::shared_ptr<Device> m_device;

		/// <summary>
		/// The image format of this swapchain.
		/// </summary>
		VkFormat m_format;

		/// <summary> The extent of this swapchain. </summary>
		VkExtent2D m_extent;

		/// <summary> The images of this swapchain. </summary>
		std::vector<VkImage> m_images;

		/// <summary> The present mode of this swapchain. </summary>
		VkPresentModeKHR m_present_mode;

		/// <summary> The surface format of this swapchain. </summary>
		VkSurfaceFormatKHR m_surface_format;

		/// <summary>
		/// Returns the optimal surface format from a list of formats.
		/// (VK_FORMAT_B8G8R8A8_SRGB) and (VK_COLOR_SPACE_SRGB_NONLINEAR_KHR).
		/// </summary>
		static VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> formats);


		/// <summary>
		/// Returns the optimal present mode from a list of modes.
		/// </summary>
		static VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR> modes);

		/// <summary>
		/// Returns the optimal Extent given the capabilities.
		/// </summary>
		static VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);

	public:

		/// <summary> Creates a swapchain for the given device and surface. </summary>
		static std::shared_ptr<Swapchain> Create(std::shared_ptr<Device> device, std::shared_ptr<Surface> surface);

		/// <summary>
		/// Gets the underlying Vulkan swapchain handle.
		/// </summary>
		VkSwapchainKHR Get() const {
			return m_swapchain;
		}

		/// <summary> Gets the format of this swapchain. </summary>
		VkFormat GetFormat() const {
			return m_format;
		}

		/// <summary> Gets the extent of this swapchain. </summary>
		VkExtent2D GetExtent() const {
			return m_extent;
		}

		/// <summary> Gets the images of this swapchain. </summary>
		std::vector<VkImage> GetVkImages() const {
			return m_images;
		}

		/// <summary> Gets the present mode of this swapchain. </summary>
		VkPresentModeKHR GetPresentMode() const {
			return m_present_mode;
		}

		/// <summary> Gets the surface format of this swapchain. </summary>
		VkSurfaceFormatKHR GetSurfaceFormat() const {
			return m_surface_format;
		}

		/// <summary> Gets the number of images in this swapchain. </summary>
		size_t Size() const {
			return m_images.size();
		}

		~Swapchain();
	};
}