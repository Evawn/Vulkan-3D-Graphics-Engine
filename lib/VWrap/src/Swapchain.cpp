#include "Swapchain.h"

namespace VWrap {

	std::shared_ptr<Swapchain> Swapchain::Create(std::shared_ptr<Device> device, std::shared_ptr<Surface> surface) {
		auto ret = std::make_shared<Swapchain>();
		ret->m_device = device;

        // Query the support, and choose optimal parameters
        SwapchainSupportDetails details = device->GetPhysicalDevice()->QuerySwapchainSupport();

        VkExtent2D extent = chooseSwapExtent(details.capabilities, *surface->GetWindow());
        VkPresentModeKHR mode = chooseSwapPresentMode(details.presentModes);
        VkSurfaceFormatKHR format = chooseSwapSurfaceFormat(details.formats);

        // Determine the minimum image count
        uint32_t imageCount = details.capabilities.minImageCount + 1;
        if (details.capabilities.maxImageCount > 0 && imageCount > details.capabilities.maxImageCount) {
            imageCount = details.capabilities.maxImageCount;
        }

        // Fill the create info
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.imageExtent = extent;
        createInfo.imageFormat = format.format;
        createInfo.presentMode = mode;
        createInfo.imageColorSpace = format.colorSpace;
        createInfo.minImageCount = imageCount;
        createInfo.imageArrayLayers = 1; // 1 unless stereoscopic
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // images used directly for rendering
        createInfo.surface = surface->Get();

        // Get the queue families, and determine whether concurrent image sharing is necessary
        QueueFamilyIndices indices = device->GetPhysicalDevice()->FindQueueFamilies();
        uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(), indices.presentFamily.value() };
        // Queue families are different
        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        // Queue families are the same
        else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0; // Optional
            createInfo.pQueueFamilyIndices = nullptr; // Optional
        }
        createInfo.preTransform = details.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        // Create the swapchain
        if (vkCreateSwapchainKHR(device->Get(), &createInfo, nullptr, &ret->m_swapchain) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create swapchain!");
        }

        // Retrieve handles to the swapchain images
        vkGetSwapchainImagesKHR(device->Get(), ret->m_swapchain, &imageCount, nullptr);
        ret->m_images.resize(imageCount);
        vkGetSwapchainImagesKHR(device->Get(), ret->m_swapchain, &imageCount, ret->m_images.data());

        ret->m_format = format.format;
        ret->m_extent = extent;
        ret->m_present_mode = mode;
        ret->m_surface_format = format;

        return ret;
	}


    VkSurfaceFormatKHR Swapchain::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> formats) {
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_R8G8B8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        return formats[0];
    }

    VkPresentModeKHR Swapchain::chooseSwapPresentMode(const std::vector<VkPresentModeKHR> modes) {
        for (const auto& mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return mode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D Swapchain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, GLFWwindow* window) {

        // We should just use the current extent, unless it takes a special value
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        // If it is a special value, then use the width/height in pixels, clamped to be within the capabilities.
        else {
            int width, height;

            // Use this function to get size in PIXELS (the WIDTH and HEIGHT properties are in screen space, not pixels).
            glfwGetFramebufferSize(window, &width, &height);
            VkExtent2D actualExtent = {
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height)
            };

            // Clamp within capabilities
            actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
            return actualExtent;
        }
    }

    Swapchain::~Swapchain() {
        std::cout << "Destroying Swapchain" << std::endl;
        if(m_swapchain != VK_NULL_HANDLE)
		    vkDestroySwapchainKHR(m_device->Get(), m_swapchain, nullptr);
	}

}