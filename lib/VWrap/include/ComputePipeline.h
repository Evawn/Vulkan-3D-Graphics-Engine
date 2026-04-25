#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include <vector>
#include "Device.h"
#include "DescriptorSetLayout.h"
#include "PipelineLayout.h"

namespace VWrap {

	class ComputePipeline {
	private:
		VkPipeline m_pipeline{ VK_NULL_HANDLE };
		std::shared_ptr<PipelineLayout> m_pipeline_layout;
		std::shared_ptr<Device> m_device;

		static VkShaderModule CreateShaderModule(std::shared_ptr<Device> device, const std::vector<char>& code);

	public:
		/// <summary>
		/// Creates a compute pipeline. Internally creates a fresh PipelineLayout from
		/// the descriptor_set_layout + push_constant_ranges arguments.
		/// </summary>
		static std::shared_ptr<ComputePipeline> Create(
			std::shared_ptr<Device> device,
			std::shared_ptr<DescriptorSetLayout> descriptor_set_layout,
			const std::vector<VkPushConstantRange>& push_constant_ranges,
			const std::vector<char>& compute_shader_code);

		/// <summary>
		/// Creates a compute pipeline against a pre-built PipelineLayout, allowing
		/// layout sharing across multiple pipelines.
		/// </summary>
		static std::shared_ptr<ComputePipeline> Create(
			std::shared_ptr<Device> device,
			std::shared_ptr<PipelineLayout> layout,
			const std::vector<char>& compute_shader_code);

		VkPipeline Get() const { return m_pipeline; }

		/// <summary> Gets the raw pipeline layout handle. </summary>
		VkPipelineLayout GetLayout() const { return m_pipeline_layout ? m_pipeline_layout->Get() : VK_NULL_HANDLE; }

		/// <summary> Gets the owning PipelineLayout wrapper. </summary>
		std::shared_ptr<PipelineLayout> GetLayoutPtr() const { return m_pipeline_layout; }

		~ComputePipeline();
	};

}
