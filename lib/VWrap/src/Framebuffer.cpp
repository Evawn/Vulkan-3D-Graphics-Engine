#include "Framebuffer.h"

namespace VWrap {
	std::shared_ptr<Framebuffer> Framebuffer::Create2D(std::shared_ptr<Device> device, std::shared_ptr<RenderPass> render_pass, std::vector<std::shared_ptr<ImageView>> attachments, VkExtent2D extent) {
		auto ret = std::make_shared<Framebuffer>();
		ret->m_device = device;
		ret->m_extent = extent;
		ret->m_render_pass = render_pass;
		ret->m_attachments = attachments;

		VkFramebufferCreateInfo createInfo{};
		
		createInfo.renderPass = render_pass->Get();
		createInfo.height = extent.height;
		createInfo.width = extent.width;
		createInfo.layers = 1;
		createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;

		std::vector<VkImageView> image_view_handles;
		for (auto& image_view : attachments) {
			image_view_handles.push_back(image_view->Get());
		}
		createInfo.attachmentCount = static_cast<uint32_t>(image_view_handles.size());
		createInfo.pAttachments = image_view_handles.data();

		if (vkCreateFramebuffer(device->Get(), &createInfo, nullptr, &ret->m_framebuffer) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create framebuffer!");
		}

		return ret;
	}

	Framebuffer::~Framebuffer() {
		if(m_framebuffer != VK_NULL_HANDLE) 
			vkDestroyFramebuffer(m_device->Get(), m_framebuffer, nullptr);
	}
}
