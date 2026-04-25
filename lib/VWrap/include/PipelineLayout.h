#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include <vector>
#include "Device.h"
#include "DescriptorSetLayout.h"

namespace VWrap {

	/// <summary>
	/// Owns a VkPipelineLayout. Keeps the descriptor-set-layout(s) it was created
	/// with alive via shared_ptr so the layout outlives any pipeline that references
	/// it. Multiple pipelines (graphics + compute, hot-reload variants) can share one
	/// PipelineLayout to avoid redundant Vulkan objects.
	/// </summary>
	class PipelineLayout {
	private:
		VkPipelineLayout m_layout{ VK_NULL_HANDLE };
		std::shared_ptr<Device> m_device;
		std::vector<std::shared_ptr<DescriptorSetLayout>> m_set_layouts;

	public:
		/// <summary>
		/// Creates a pipeline layout from one or more descriptor set layouts and an
		/// optional list of push constant ranges. The set layouts are bound by index
		/// in the order provided.
		/// </summary>
		static std::shared_ptr<PipelineLayout> Create(
			std::shared_ptr<Device> device,
			std::vector<std::shared_ptr<DescriptorSetLayout>> setLayouts,
			const std::vector<VkPushConstantRange>& pushConstantRanges = {});

		/// <summary> Convenience: create from a single set layout. </summary>
		static std::shared_ptr<PipelineLayout> Create(
			std::shared_ptr<Device> device,
			std::shared_ptr<DescriptorSetLayout> setLayout,
			const std::vector<VkPushConstantRange>& pushConstantRanges = {});

		VkPipelineLayout Get() const { return m_layout; }

		~PipelineLayout();
	};

}
