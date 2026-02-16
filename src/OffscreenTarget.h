#pragma once

#include "Device.h"
#include "Allocator.h"
#include "Image.h"
#include "ImageView.h"
#include "Sampler.h"
#include "RenderPass.h"
#include "Framebuffer.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

class OffscreenTarget {
private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<VWrap::RenderPass> m_render_pass;

	std::shared_ptr<VWrap::ImageView> m_color_view;
	std::shared_ptr<VWrap::ImageView> m_depth_view;
	std::shared_ptr<VWrap::Image> m_resolve_image;
	std::shared_ptr<VWrap::ImageView> m_resolve_view;
	std::shared_ptr<VWrap::Framebuffer> m_framebuffer;
	std::shared_ptr<VWrap::Sampler> m_sampler;

	VkDescriptorSet m_imgui_texture = VK_NULL_HANDLE;
	VkExtent2D m_extent{};
	VkFormat m_color_format;

	void CreateResources(VkSampleCountFlagBits samples);

public:
	static std::shared_ptr<OffscreenTarget> Create(
		std::shared_ptr<VWrap::Device> device,
		std::shared_ptr<VWrap::Allocator> allocator,
		std::shared_ptr<VWrap::RenderPass> renderPass,
		VkExtent2D extent,
		VkSampleCountFlagBits samples,
		VkFormat colorFormat);

	void Resize(VkExtent2D newExtent);

	VkDescriptorSet GetImGuiTextureID() const { return m_imgui_texture; }
	std::shared_ptr<VWrap::Framebuffer> GetFramebuffer() const { return m_framebuffer; }
	std::shared_ptr<VWrap::RenderPass> GetRenderPass() const { return m_render_pass; }
	VkExtent2D GetExtent() const { return m_extent; }
	std::shared_ptr<VWrap::Image> GetResolveImage() const { return m_resolve_image; }
	VkFormat GetColorFormat() const { return m_color_format; }
};
