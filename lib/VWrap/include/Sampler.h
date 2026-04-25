#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan sampler object.
	/// </summary>
	class Sampler
	{
	private:

		/// <summary> The underlying Vulkan sampler object. </summary>
		VkSampler m_sampler{ VK_NULL_HANDLE };

		/// <summary> The device that created this sampler. </summary>
		std::shared_ptr<Device> m_device;

	public:

		/// <summary>
		/// Creates a sampler from an explicit VkSamplerCreateInfo. Use this when none
		/// of the named factories below capture the configuration you need.
		/// </summary>
		static std::shared_ptr<Sampler> Create(std::shared_ptr<Device> device, const VkSamplerCreateInfo& info);

		/// <summary>
		/// Linear filter, REPEAT addressing, anisotropic filtering at device max.
		/// Default for diffuse textures and other tiled inputs. (Equivalent to the
		/// previous bare Sampler::Create(device).)
		/// </summary>
		static std::shared_ptr<Sampler> CreateLinearRepeat(std::shared_ptr<Device> device);

		/// <summary>
		/// Linear filter, CLAMP_TO_EDGE addressing, no anisotropy. Recommended for
		/// post-process intermediates (bloom, tonemap, etc.) where edge wrapping
		/// would create artifacts and anisotropy is wasted on screen-space taps.
		/// </summary>
		static std::shared_ptr<Sampler> CreateLinearClamp(std::shared_ptr<Device> device);

		/// <summary>
		/// Nearest filter, CLAMP_TO_EDGE addressing, no anisotropy. Required for
		/// integer-format storage images (e.g. R8_UINT voxel volumes) which do not
		/// support linear filtering.
		/// </summary>
		static std::shared_ptr<Sampler> CreateNearestClamp(std::shared_ptr<Device> device);

		/// <summary>
		/// Convenience: the default Create(device) overload preserves the prior
		/// behavior (linear + repeat + anisotropic) so existing call sites compile
		/// unchanged.
		/// </summary>
		static std::shared_ptr<Sampler> Create(std::shared_ptr<Device> device) { return CreateLinearRepeat(device); }

		/// <summary> Gets the underlying Vulkan sampler object. </summary>
		VkSampler Get() const { return m_sampler; }

		~Sampler();
	};
}
