#pragma once
#include "vulkan/vulkan.h"
#include <memory>
#include "Device.h"
#include "RenderPass.h"
#include "DescriptorSetLayout.h"
#include "PipelineLayout.h"
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
		uint32_t colorAttachmentCount = 1;
	};

	/// <summary>
	/// Represents a Vulkan graphics pipeline. Owns its VkPipeline and a shared_ptr
	/// to a PipelineLayout (which owns the VkPipelineLayout). Multiple pipelines
	/// can share one layout via the PipelineLayout overload of Create.
	/// </summary>
	class Pipeline {
	private:

		/// <summary> The pipeline handle. </summary>
		VkPipeline m_pipeline{ VK_NULL_HANDLE };

		/// <summary> The shared pipeline layout (owns the VkPipelineLayout). </summary>
		std::shared_ptr<PipelineLayout> m_pipeline_layout;

		/// <summary> The device that created the pipeline. </summary>
		std::shared_ptr<Device> m_device;

		static VkShaderModule CreateShaderModule(std::shared_ptr<Device> device, const std::vector<char>& code);

	public:

		/// <summary>
		/// Creates a graphics pipeline. Internally creates a fresh PipelineLayout
		/// from the descriptor_set_layout + push_constant_ranges in create_info.
		/// </summary>
		static std::shared_ptr<Pipeline> Create(std::shared_ptr<Device> device, const PipelineCreateInfo& create_info, const std::vector<char>& vertex_shader_code, const std::vector<char>& fragment_shader_code);

		/// <summary>
		/// Creates a graphics pipeline against a pre-built PipelineLayout. Use this
		/// to share one layout across multiple pipelines (graphics + compute, hot-
		/// reload variants, MRT/non-MRT pairs, etc.).
		/// </summary>
		static std::shared_ptr<Pipeline> Create(std::shared_ptr<Device> device, std::shared_ptr<PipelineLayout> layout, const PipelineCreateInfo& create_info, const std::vector<char>& vertex_shader_code, const std::vector<char>& fragment_shader_code);

		VkPipeline Get() const { return m_pipeline; }

		/// <summary> Gets the raw pipeline layout handle (for vkCmd... calls). </summary>
		VkPipelineLayout GetLayout() const { return m_pipeline_layout ? m_pipeline_layout->Get() : VK_NULL_HANDLE; }

		/// <summary> Gets the owning PipelineLayout wrapper (for sharing across pipelines). </summary>
		std::shared_ptr<PipelineLayout> GetLayoutPtr() const { return m_pipeline_layout; }

		~Pipeline();
	};
}
