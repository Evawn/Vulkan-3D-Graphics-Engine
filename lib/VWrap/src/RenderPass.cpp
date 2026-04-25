#include "RenderPass.h"

namespace VWrap {

	std::shared_ptr<RenderPass> RenderPass::CreatePresentation(std::shared_ptr<Device> device, VkFormat swapchainFormat) {
		auto ret = std::make_shared<RenderPass>();
		ret->m_device = device;
		ret->m_samples = VK_SAMPLE_COUNT_1_BIT;

		VkAttachmentDescription colorAttachment{};
		colorAttachment.format = swapchainFormat;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorRef{};
		colorRef.attachment = 0;
		colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		createInfo.attachmentCount = 1;
		createInfo.pAttachments = &colorAttachment;
		createInfo.subpassCount = 1;
		createInfo.pSubpasses = &subpass;
		createInfo.dependencyCount = 1;
		createInfo.pDependencies = &dependency;

		if (vkCreateRenderPass(device->Get(), &createInfo, nullptr, &ret->m_render_pass) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create presentation render pass!");
		}

		return ret;
	}

	std::shared_ptr<RenderPass> RenderPass::Create(std::shared_ptr<Device> device, const VkRenderPassCreateInfo& createInfo, VkSampleCountFlagBits samples) {
		auto ret = std::make_shared<RenderPass>();
		ret->m_device = device;
		ret->m_samples = samples;

		if (vkCreateRenderPass(device->Get(), &createInfo, nullptr, &ret->m_render_pass) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create render pass!");
		}

		return ret;
	}

	RenderPass::~RenderPass() {
		if (m_render_pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(m_device->Get(), m_render_pass, nullptr);
	}
}
