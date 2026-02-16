#include "ComputeTest.h"
#include "config.h"
#include <spdlog/spdlog.h>

struct ComputeTestPC {
	float width;
	float height;
};

void ComputeTest::Init(const RenderContext& ctx) {
	auto logger = spdlog::get("Render");
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_extent = ctx.extent;
	m_graphics_pool = ctx.graphicsPool;
	m_compute_pool = ctx.computePool;
	m_render_pass = ctx.renderPass;

	logger->debug("ComputeTest: Creating storage image...");
	CreateStorageImage();

	logger->debug("ComputeTest: Creating compute descriptors...");
	CreateComputeDescriptors();

	logger->debug("ComputeTest: Creating compute pipeline...");
	CreateComputePipeline();

	logger->debug("ComputeTest: Creating graphics descriptors...");
	CreateGraphicsDescriptors(ctx.maxFramesInFlight);

	logger->debug("ComputeTest: Creating graphics pipeline...");
	CreateGraphicsPipeline();

	logger->debug("ComputeTest: Creating sampler...");
	m_sampler = VWrap::Sampler::Create(m_device);

	logger->debug("ComputeTest: Writing descriptors...");
	WriteDescriptors();

	logger->debug("ComputeTest: Dispatching compute...");
	DispatchCompute();

	logger->debug("ComputeTest: Initialized");
}

void ComputeTest::CreateStorageImage() {
	VWrap::ImageCreateInfo info{};
	info.width = m_extent.width;
	info.height = m_extent.height;
	info.depth = 1;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	info.mip_levels = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.image_type = VK_IMAGE_TYPE_2D;

	m_storage_image = VWrap::Image::Create(m_allocator, info);
	m_storage_image_view = VWrap::ImageView::Create(m_device, m_storage_image);
}

void ComputeTest::CreateComputeDescriptors() {
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorCount = 1;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	m_compute_descriptor_layout = VWrap::DescriptorSetLayout::Create(m_device, { binding });

	std::vector<VkDescriptorPoolSize> poolSizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }
	};
	m_compute_descriptor_pool = VWrap::DescriptorPool::Create(m_device, poolSizes, 1, 0);

	m_compute_descriptor_set = VWrap::DescriptorSet::Create(m_compute_descriptor_pool, m_compute_descriptor_layout);
}

void ComputeTest::CreateComputePipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/compute_test.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(ComputeTestPC);

	m_compute_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_compute_descriptor_layout, { pushRange }, comp_code);
}

void ComputeTest::CreateGraphicsDescriptors(uint32_t max_sets) {
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorCount = 1;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	m_graphics_descriptor_layout = VWrap::DescriptorSetLayout::Create(m_device, { binding });

	std::vector<VkDescriptorPoolSize> poolSizes = {
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_sets }
	};
	m_graphics_descriptor_pool = VWrap::DescriptorPool::Create(m_device, poolSizes, max_sets, 0);

	std::vector<std::shared_ptr<VWrap::DescriptorSetLayout>> layouts(max_sets, m_graphics_descriptor_layout);
	m_graphics_descriptor_sets = VWrap::DescriptorSet::CreateMany(m_graphics_descriptor_pool, layouts);
}

void ComputeTest::CreateGraphicsPipeline() {
	auto vert_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/compute_test.vert.spv");
	auto frag_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/compute_test.frag.spv");

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexAttributeDescriptionCount = 0;
	vertexInputInfo.vertexBindingDescriptionCount = 0;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	inputAssembly.primitiveRestartEnable = VK_TRUE;

	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VWrap::PipelineCreateInfo create_info{};
	create_info.extent = m_extent;
	create_info.render_pass = m_render_pass;
	create_info.descriptor_set_layout = m_graphics_descriptor_layout;
	create_info.vertex_input_info = vertexInputInfo;
	create_info.input_assembly = inputAssembly;
	create_info.dynamic_state = dynamicState;
	create_info.rasterizer = rasterizer;
	create_info.depth_stencil = depthStencil;
	create_info.subpass = 0;

	m_graphics_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_code, frag_code);
}

void ComputeTest::WriteDescriptors() {
	// Compute descriptor: storage image in GENERAL layout
	VkDescriptorImageInfo computeImageInfo{};
	computeImageInfo.imageView = m_storage_image_view->Get();
	computeImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	VkWriteDescriptorSet computeWrite{};
	computeWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	computeWrite.dstSet = m_compute_descriptor_set->Get();
	computeWrite.dstBinding = 0;
	computeWrite.descriptorCount = 1;
	computeWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	computeWrite.pImageInfo = &computeImageInfo;

	vkUpdateDescriptorSets(m_device->Get(), 1, &computeWrite, 0, nullptr);

	// Graphics descriptors: sampled image per frame
	for (auto& ds : m_graphics_descriptor_sets) {
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = m_storage_image_view->Get();
		imageInfo.sampler = m_sampler->Get();

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = ds->Get();
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(m_device->Get(), 1, &write, 0, nullptr);
	}
}

void ComputeTest::DispatchCompute() {
	auto cmd = VWrap::CommandBuffer::Create(m_compute_pool);
	cmd->BeginSingle();

	// Transition: UNDEFINED -> GENERAL (for compute write)
	cmd->CmdTransitionImageLayout(m_storage_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	// Bind compute pipeline and descriptor
	cmd->CmdBindComputePipeline(m_compute_pipeline);
	cmd->CmdBindComputeDescriptorSets(m_compute_pipeline->GetLayout(),
		{ m_compute_descriptor_set->Get() });

	// Push constants: image dimensions
	ComputeTestPC pc{};
	pc.width = static_cast<float>(m_extent.width);
	pc.height = static_cast<float>(m_extent.height);
	vkCmdPushConstants(cmd->Get(), m_compute_pipeline->GetLayout(),
		VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

	// Dispatch: workgroup size 16x16, round up
	uint32_t groupX = (m_extent.width + 15) / 16;
	uint32_t groupY = (m_extent.height + 15) / 16;
	cmd->CmdDispatch(groupX, groupY, 1);

	// Transition: GENERAL -> SHADER_READ_ONLY (for fragment read)
	cmd->CmdTransitionImageLayout(m_storage_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	cmd->EndAndSubmit();
}

void ComputeTest::RecordCommands(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex, std::shared_ptr<Camera> camera) {
	auto vk_cmd = cmd->Get();
	vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline->Get());

	VkDescriptorSet ds = m_graphics_descriptor_sets[frameIndex]->Get();
	vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		m_graphics_pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

	VkViewport viewport{};
	viewport.width = static_cast<float>(m_extent.width);
	viewport.height = static_cast<float>(m_extent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	viewport.x = 0;
	viewport.y = 0;
	vkCmdSetViewport(vk_cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = m_extent;
	scissor.offset = { 0, 0 };
	vkCmdSetScissor(vk_cmd, 0, 1, &scissor);

	vkCmdDraw(vk_cmd, 4, 1, 0, 0);
}

void ComputeTest::OnResize(VkExtent2D newExtent) {
	m_extent = newExtent;
	m_device->WaitIdle();
	CreateStorageImage();
	WriteDescriptors();
	DispatchCompute();
}

std::vector<std::string> ComputeTest::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/compute_test.comp.spv",
		std::string(config::SHADER_DIR) + "/compute_test.vert.spv",
		std::string(config::SHADER_DIR) + "/compute_test.frag.spv"
	};
}

void ComputeTest::RecreatePipeline(const RenderContext& ctx) {
	m_graphics_pipeline.reset();
	m_compute_pipeline.reset();
	CreateComputePipeline();
	CreateGraphicsPipeline();
	DispatchCompute();
}

FrameStats ComputeTest::GetFrameStats() const {
	return { 1, 4, 0 };
}
