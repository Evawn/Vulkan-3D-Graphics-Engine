#pragma once

#include "Device.h"
#include "Allocator.h"
#include "Image.h"
#include "ImageView.h"
#include "Sampler.h"
#include "RenderPass.h"
#include "Framebuffer.h"

namespace VWrap {

class OffscreenTarget {
private:
	std::shared_ptr<Device> m_device;
	std::shared_ptr<Allocator> m_allocator;
	std::shared_ptr<RenderPass> m_render_pass;

	std::shared_ptr<ImageView> m_color_view;
	std::shared_ptr<ImageView> m_depth_view;
	std::shared_ptr<Image> m_resolve_image;
	std::shared_ptr<ImageView> m_resolve_view;
	std::shared_ptr<Framebuffer> m_framebuffer;
	std::shared_ptr<Sampler> m_sampler;

	VkExtent2D m_extent{};
	VkFormat m_color_format;

	void CreateResources(VkSampleCountFlagBits samples);

public:
	static std::shared_ptr<OffscreenTarget> Create(
		std::shared_ptr<Device> device,
		std::shared_ptr<Allocator> allocator,
		std::shared_ptr<RenderPass> renderPass,
		VkExtent2D extent,
		VkSampleCountFlagBits samples,
		VkFormat colorFormat);

	void Resize(VkExtent2D newExtent);

	std::shared_ptr<Framebuffer> GetFramebuffer() const { return m_framebuffer; }
	std::shared_ptr<RenderPass> GetRenderPass() const { return m_render_pass; }
	VkExtent2D GetExtent() const { return m_extent; }
	std::shared_ptr<Image> GetResolveImage() const { return m_resolve_image; }
	std::shared_ptr<ImageView> GetResolveView() const { return m_resolve_view; }
	std::shared_ptr<Sampler> GetSampler() const { return m_sampler; }
	VkFormat GetColorFormat() const { return m_color_format; }
};

} // namespace VWrap
