#include "PaletteResource.h"
#include "DefaultPalette.h"
#include "Buffer.h"
#include "CommandBuffer.h"
#include <cstring>

PaletteResource::PaletteResource(
	std::shared_ptr<VWrap::Device> device,
	std::shared_ptr<VWrap::Allocator> allocator,
	std::shared_ptr<VWrap::CommandPool> graphicsPool)
	: m_device(std::move(device))
	, m_allocator(std::move(allocator))
	, m_graphics_pool(std::move(graphicsPool))
{}

void PaletteResource::Create() {
	VWrap::ImageCreateInfo info{};
	info.width = 256;
	info.height = 1;
	info.depth = 1;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	info.mip_levels = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.image_type = VK_IMAGE_TYPE_2D;

	m_image = VWrap::Image::Create(m_allocator, info);

	// Initial transition UNDEFINED -> SHADER_READ_ONLY_OPTIMAL so Upload() can
	// always assume the image is in the shader-read layout going in.
	auto cmd = VWrap::CommandBuffer::Create(m_graphics_pool);
	cmd->BeginSingle();
	cmd->CmdTransitionImageLayout(m_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	cmd->EndAndSubmit();

	m_image_view = VWrap::ImageView::Create(m_device, m_image, VK_IMAGE_ASPECT_COLOR_BIT);
	m_sampler = VWrap::Sampler::Create(m_device);

	RestoreDefault();
}

void PaletteResource::RestoreDefault() {
	auto palette = BuildDefaultPalette();
	Upload(palette.data());
}

void PaletteResource::Upload(const uint8_t* rgbaData) {
	VkDeviceSize imageSize = 256 * 4;

	auto staging = VWrap::Buffer::CreateStaging(m_allocator, imageSize);
	void* mapped = staging->Map();
	std::memcpy(mapped, rgbaData, imageSize);
	staging->Unmap();

	auto cmd = VWrap::CommandBuffer::Create(m_graphics_pool);
	cmd->BeginSingle();
	cmd->CmdTransitionImageLayout(m_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	cmd->CmdCopyBufferToImage(staging, m_image, 256, 1, 1);
	cmd->CmdTransitionImageLayout(m_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	cmd->EndAndSubmit();
}
