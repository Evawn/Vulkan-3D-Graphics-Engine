#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include "Pipeline.h"

namespace PipelineDefaults {

// Dynamic viewport and scissor state (used by virtually all pipelines)
inline VkPipelineDynamicStateCreateInfo DynamicViewportScissor() {
	static std::array<VkDynamicState, 2> states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	info.dynamicStateCount = static_cast<uint32_t>(states.size());
	info.pDynamicStates = states.data();
	return info;
}

// Empty vertex input (for fullscreen quad passes)
inline VkPipelineVertexInputStateCreateInfo NoVertexInput() {
	VkPipelineVertexInputStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	info.vertexAttributeDescriptionCount = 0;
	info.vertexBindingDescriptionCount = 0;
	return info;
}

// Triangle strip assembly (for fullscreen quads)
inline VkPipelineInputAssemblyStateCreateInfo TriangleStrip() {
	VkPipelineInputAssemblyStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	info.primitiveRestartEnable = VK_TRUE;
	return info;
}

// Triangle list assembly (for mesh rendering)
inline VkPipelineInputAssemblyStateCreateInfo TriangleList() {
	VkPipelineInputAssemblyStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	info.primitiveRestartEnable = VK_FALSE;
	return info;
}

// No-cull fill rasterizer (for fullscreen quads)
inline VkPipelineRasterizationStateCreateInfo NoCullFill() {
	VkPipelineRasterizationStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	info.depthClampEnable = VK_FALSE;
	info.rasterizerDiscardEnable = VK_FALSE;
	info.polygonMode = VK_POLYGON_MODE_FILL;
	info.lineWidth = 1.0f;
	info.cullMode = VK_CULL_MODE_NONE;
	info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	info.depthBiasEnable = VK_FALSE;
	return info;
}

// Back-face cull rasterizer with optional wireframe
inline VkPipelineRasterizationStateCreateInfo BackCullFill(bool wireframe = false) {
	VkPipelineRasterizationStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	info.depthClampEnable = VK_FALSE;
	info.rasterizerDiscardEnable = VK_FALSE;
	info.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	info.lineWidth = 1.0f;
	info.cullMode = VK_CULL_MODE_BACK_BIT;
	info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	info.depthBiasEnable = VK_FALSE;
	return info;
}

// Depth test disabled (for fullscreen quads / 2D passes)
inline VkPipelineDepthStencilStateCreateInfo NoDepthTest() {
	VkPipelineDepthStencilStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	info.depthTestEnable = VK_FALSE;
	info.depthWriteEnable = VK_FALSE;
	info.depthCompareOp = VK_COMPARE_OP_LESS;
	info.depthBoundsTestEnable = VK_FALSE;
	info.stencilTestEnable = VK_FALSE;
	return info;
}

// Standard depth test with write
inline VkPipelineDepthStencilStateCreateInfo DepthTestWrite() {
	VkPipelineDepthStencilStateCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	info.depthTestEnable = VK_TRUE;
	info.depthWriteEnable = VK_TRUE;
	info.depthCompareOp = VK_COMPARE_OP_LESS;
	info.depthBoundsTestEnable = VK_FALSE;
	info.stencilTestEnable = VK_FALSE;
	return info;
}

// Pre-configured PipelineCreateInfo for a fullscreen quad pass
inline VWrap::PipelineCreateInfo FullscreenQuad(
	std::shared_ptr<VWrap::RenderPass> renderPass,
	std::shared_ptr<VWrap::DescriptorSetLayout> layout,
	VkExtent2D extent,
	std::vector<VkPushConstantRange> pushConstants = {})
{
	VWrap::PipelineCreateInfo info{};
	info.extent = extent;
	info.render_pass = renderPass;
	info.descriptor_set_layout = layout;
	info.vertex_input_info = NoVertexInput();
	info.input_assembly = TriangleStrip();
	info.dynamic_state = DynamicViewportScissor();
	info.rasterizer = NoCullFill();
	info.depth_stencil = NoDepthTest();
	info.push_constant_ranges = std::move(pushConstants);
	info.subpass = 0;
	return info;
}

} // namespace PipelineDefaults
