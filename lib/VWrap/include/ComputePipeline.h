#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include <vector>
#include "Device.h"
#include "DescriptorSetLayout.h"

namespace VWrap {

	class ComputePipeline {
	private:
		VkPipeline m_pipeline{ VK_NULL_HANDLE };
		VkPipelineLayout m_pipeline_layout{ VK_NULL_HANDLE };
		std::shared_ptr<Device> m_device;

		static VkShaderModule CreateShaderModule(std::shared_ptr<Device> device, const std::vector<char>& code);

	public:
		static std::shared_ptr<ComputePipeline> Create(
			std::shared_ptr<Device> device,
			std::shared_ptr<DescriptorSetLayout> descriptor_set_layout,
			const std::vector<VkPushConstantRange>& push_constant_ranges,
			const std::vector<char>& compute_shader_code);

		VkPipeline Get() const { return m_pipeline; }
		VkPipelineLayout GetLayout() const { return m_pipeline_layout; }

		~ComputePipeline();
	};

}
