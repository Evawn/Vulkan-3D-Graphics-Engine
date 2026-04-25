#include "ComputePipeline.h"

namespace VWrap {

	std::shared_ptr<ComputePipeline> ComputePipeline::Create(
		std::shared_ptr<Device> device,
		std::shared_ptr<DescriptorSetLayout> descriptor_set_layout,
		const std::vector<VkPushConstantRange>& push_constant_ranges,
		const std::vector<char>& compute_shader_code)
	{
		auto layout = PipelineLayout::Create(device, std::move(descriptor_set_layout), push_constant_ranges);
		return Create(std::move(device), std::move(layout), compute_shader_code);
	}

	std::shared_ptr<ComputePipeline> ComputePipeline::Create(
		std::shared_ptr<Device> device,
		std::shared_ptr<PipelineLayout> layout,
		const std::vector<char>& compute_shader_code)
	{
		auto ret = std::make_shared<ComputePipeline>();
		ret->m_device = device;
		ret->m_pipeline_layout = std::move(layout);

		VkShaderModule compModule = CreateShaderModule(device, compute_shader_code);

		VkPipelineShaderStageCreateInfo stageInfo{};
		stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stageInfo.module = compModule;
		stageInfo.pName = "main";

		VkComputePipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipelineInfo.stage = stageInfo;
		pipelineInfo.layout = ret->m_pipeline_layout->Get();

		if (vkCreateComputePipelines(device->Get(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &ret->m_pipeline) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create compute pipeline!");
		}

		vkDestroyShaderModule(device->Get(), compModule, nullptr);

		return ret;
	}

	VkShaderModule ComputePipeline::CreateShaderModule(std::shared_ptr<Device> device, const std::vector<char>& code) {
		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = code.size();
		createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

		VkShaderModule shaderModule;
		if (vkCreateShaderModule(device->Get(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create shader module!");
		}
		return shaderModule;
	}

	ComputePipeline::~ComputePipeline() {
		if (m_pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device->Get(), m_pipeline, nullptr);
		// m_pipeline_layout is a shared_ptr — destruction is handled by PipelineLayout itself.
	}

}
