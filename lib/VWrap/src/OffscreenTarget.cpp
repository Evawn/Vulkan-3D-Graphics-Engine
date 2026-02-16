#include "OffscreenTarget.h"
#include <spdlog/spdlog.h>

namespace VWrap {

std::shared_ptr<OffscreenTarget> OffscreenTarget::Create(
	std::shared_ptr<Device> device,
	std::shared_ptr<Allocator> allocator,
	std::shared_ptr<RenderPass> renderPass,
	VkExtent2D extent,
	VkSampleCountFlagBits samples,
	VkFormat colorFormat)
{
	auto ret = std::make_shared<OffscreenTarget>();
	ret->m_device = device;
	ret->m_allocator = allocator;
	ret->m_render_pass = renderPass;
	ret->m_extent = extent;
	ret->m_color_format = colorFormat;
	ret->m_sampler = Sampler::Create(device);

	ret->CreateResources(samples);
	return ret;
}

void OffscreenTarget::CreateResources(VkSampleCountFlagBits samples) {
	// MSAA color attachment
	ImageCreateInfo colorInfo{};
	colorInfo.width = m_extent.width;
	colorInfo.height = m_extent.height;
	colorInfo.depth = 1;
	colorInfo.format = m_color_format;
	colorInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	colorInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
	colorInfo.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	colorInfo.mip_levels = 1;
	colorInfo.samples = samples;
	colorInfo.image_type = VK_IMAGE_TYPE_2D;

	auto colorImage = Image::Create(m_allocator, colorInfo);
	m_color_view = ImageView::Create(m_device, colorImage, VK_IMAGE_ASPECT_COLOR_BIT);

	// Depth attachment
	VkFormat depthFormat = FindDepthFormat(m_device->GetPhysicalDevice()->Get());
	ImageCreateInfo depthInfo{};
	depthInfo.width = m_extent.width;
	depthInfo.height = m_extent.height;
	depthInfo.depth = 1;
	depthInfo.format = depthFormat;
	depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depthInfo.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	depthInfo.mip_levels = 1;
	depthInfo.samples = samples;
	depthInfo.image_type = VK_IMAGE_TYPE_2D;

	auto depthImage = Image::Create(m_allocator, depthInfo);
	m_depth_view = ImageView::Create(m_device, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT);

	// Resolve attachment (1x, sampled)
	ImageCreateInfo resolveInfo{};
	resolveInfo.width = m_extent.width;
	resolveInfo.height = m_extent.height;
	resolveInfo.depth = 1;
	resolveInfo.format = m_color_format;
	resolveInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	resolveInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	resolveInfo.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	resolveInfo.mip_levels = 1;
	resolveInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	resolveInfo.image_type = VK_IMAGE_TYPE_2D;

	m_resolve_image = Image::Create(m_allocator, resolveInfo);
	m_resolve_view = ImageView::Create(m_device, m_resolve_image, VK_IMAGE_ASPECT_COLOR_BIT);

	// Framebuffer: [MSAA color, depth, resolve]
	std::vector<std::shared_ptr<ImageView>> attachments = {
		m_color_view,
		m_depth_view,
		m_resolve_view
	};
	m_framebuffer = Framebuffer::Create2D(m_device, m_render_pass, attachments, m_extent);

	spdlog::get("App")->info("Created offscreen target {}x{}", m_extent.width, m_extent.height);
}

void OffscreenTarget::Resize(VkExtent2D newExtent) {
	if (newExtent.width == 0 || newExtent.height == 0) return;
	if (newExtent.width == m_extent.width && newExtent.height == m_extent.height) return;

	vkDeviceWaitIdle(m_device->Get());

	// Destroy old resources (RAII will handle cleanup when shared_ptrs are reassigned)
	m_framebuffer.reset();
	m_resolve_view.reset();
	m_resolve_image.reset();
	m_depth_view.reset();
	m_color_view.reset();

	m_extent = newExtent;
	CreateResources(m_render_pass->GetSamples());

	spdlog::get("App")->info("Resized offscreen target to {}x{}", m_extent.width, m_extent.height);
}

} // namespace VWrap
