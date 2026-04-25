#pragma once

#include <cstdint>
#include <memory>
#include "Device.h"
#include "Allocator.h"
#include "CommandPool.h"
#include "Image.h"
#include "ImageView.h"
#include "Sampler.h"

// Owns a 256-entry RGBA8 palette texture (256x1, R8G8B8A8_UNORM) plus the sampler
// used to read it from a fragment shader. Shared between voxel render techniques
// that store material indices per voxel and look up colors via texelFetch.
class PaletteResource {
public:
	PaletteResource(
		std::shared_ptr<VWrap::Device> device,
		std::shared_ptr<VWrap::Allocator> allocator,
		std::shared_ptr<VWrap::CommandPool> graphicsPool);

	// Build the palette image, view, sampler, and seed it with the engine's
	// default colors (index 0 = empty, 1-9 = shape colors, 10-255 = HSV rainbow).
	void Create();

	// Re-upload a 256x4-byte RGBA palette over the existing image (no new VkResources).
	// Used when a .vox import provides its own palette.
	void Upload(const uint8_t* rgbaData);

	// Re-upload the default palette over the existing image. Used to restore
	// procedural rendering after a .vox model is unloaded.
	void RestoreDefault();

	std::shared_ptr<VWrap::ImageView> GetImageView() const { return m_image_view; }
	std::shared_ptr<VWrap::Sampler> GetSampler() const { return m_sampler; }

private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;

	std::shared_ptr<VWrap::Image> m_image;
	std::shared_ptr<VWrap::ImageView> m_image_view;
	std::shared_ptr<VWrap::Sampler> m_sampler;
};
