#include "PaletteResource.h"
#include "Buffer.h"
#include "CommandBuffer.h"
#include <array>
#include <cmath>
#include <cstring>

// HSV (h in [0,360), s,v in [0,1]) -> RGB bytes.
static void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
	float c = v * s;
	float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
	float m = v - c;
	float rf, gf, bf;
	if (h < 60)       { rf = c; gf = x; bf = 0; }
	else if (h < 120) { rf = x; gf = c; bf = 0; }
	else if (h < 180) { rf = 0; gf = c; bf = x; }
	else if (h < 240) { rf = 0; gf = x; bf = c; }
	else if (h < 300) { rf = x; gf = 0; bf = c; }
	else              { rf = c; gf = 0; bf = x; }
	r = static_cast<uint8_t>((rf + m) * 255.0f);
	g = static_cast<uint8_t>((gf + m) * 255.0f);
	b = static_cast<uint8_t>((bf + m) * 255.0f);
}

PaletteResource::PaletteResource(
	std::shared_ptr<VWrap::Device> device,
	std::shared_ptr<VWrap::Allocator> allocator,
	std::shared_ptr<VWrap::CommandPool> graphicsPool)
	: m_device(std::move(device))
	, m_allocator(std::move(allocator))
	, m_graphics_pool(std::move(graphicsPool))
{}

// Default palette: index 0 empty, 1-9 hardcoded shape colors, 10-255 HSV rainbow.
static std::array<uint8_t, 256 * 4> BuildDefaultPalette() {
	std::array<uint8_t, 256 * 4> palette{};

	const uint8_t colors[][4] = {
		{   0,   0,   0,   0 },   // 0: empty (never sampled)
		{ 230,  60,  60, 255 },   // 1: red (sphere)
		{  60, 180,  60, 255 },   // 2: green (torus)
		{  60,  60, 230, 255 },   // 3: blue (box frame)
		{ 230, 230,  60, 255 },   // 4: yellow (cylinder)
		{ 230, 130,  60, 255 },   // 5: orange (cone)
		{ 180,  60, 230, 255 },   // 6: purple (octahedron)
		{  60, 230, 230, 255 },   // 7: cyan (gyroid)
		{ 230,  60, 180, 255 },   // 8: pink (sine blob)
		{ 180, 180, 180, 255 },   // 9: gray (menger sponge)
	};

	for (int i = 0; i < 10; i++) {
		std::memcpy(&palette[i * 4], colors[i], 4);
	}
	for (int i = 10; i < 256; i++) {
		float hue = static_cast<float>(i - 10) / 246.0f * 360.0f;
		hsvToRgb(hue, 0.85f, 0.95f, palette[i * 4 + 0], palette[i * 4 + 1], palette[i * 4 + 2]);
		palette[i * 4 + 3] = 255;
	}
	return palette;
}

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
