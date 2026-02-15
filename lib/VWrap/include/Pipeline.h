#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "RenderPass.h"
#include "DescriptorSetLayout.h"
#include "Utils.h"
#include <array>

namespace VWrap {

	struct PipelineCreateInfo {
		VkExtent2D extent;
		std::shared_ptr<RenderPass> render_pass;
		std::shared_ptr<DescriptorSetLayout> descriptor_set_layout;
		VkPipelineVertexInputStateCreateInfo vertex_input_info;
		VkPipelineInputAssemblyStateCreateInfo input_assembly;
		VkPipelineDynamicStateCreateInfo dynamic_state;
		VkPipelineRasterizationStateCreateInfo rasterizer;
		VkPipelineDepthStencilStateCreateInfo depth_stencil;

		std::vector<VkPushConstantRange> push_constant_ranges;
		uint32_t subpass;
	};

	/// <summary>
	/// Represents a Vulkan pipeline.
	/// </summary>
	class Pipeline {
	private:

		/// <summary> The pipeline handle. </summary>
		VkPipeline m_pipeline;

		/// <summary> The pipeline layout handle. </summary>
		VkPipelineLayout m_pipeline_layout;

		/// <summary> The device that created the pipeline. </summary>
		std::shared_ptr<Device> m_device;

		/// <summary>
		/// Creates a shader module from the given code.
		/// </summary>
		static VkShaderModule CreateShaderModule(std::shared_ptr<Device> device, const std::vector<char>& code);

	public:

		static std::shared_ptr<Pipeline> Create(std::shared_ptr<Device> device, const PipelineCreateInfo& create_info, const std::vector<char>& vertex_shader_code, const std::vector<char>& fragment_shader_code);

		VkPipeline Get() const { return m_pipeline; }

		/// <summary> Gets the pipeline layout handle. </summary>
		VkPipelineLayout GetLayout() const { return m_pipeline_layout; }

		~Pipeline();
	};
}