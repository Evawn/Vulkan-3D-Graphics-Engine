#include "OctreeTracer.h"
#include "config.h"

std::shared_ptr<OctreeTracer> OctreeTracer::Create(std::shared_ptr<VWrap::Allocator> allocator, std::shared_ptr<VWrap::Device> device, std::shared_ptr<VWrap::RenderPass> render_pass, std::shared_ptr<VWrap::CommandPool> graphics_pool, VkExtent2D extent, uint32_t num_frames) {
	auto ret = std::make_shared<OctreeTracer>();
	ret->m_device = device;
	ret->m_allocator = allocator;
	ret->m_extent = extent;
	ret->m_graphics_pool = graphics_pool;

	ret->CreateDescriptors(num_frames);
	ret->CreatePipeline(render_pass);

	ret->m_sampler = VWrap::Sampler::Create(device);
	VWrap::CommandBuffer::CreateAndFillBrickTexture(graphics_pool, allocator, ret->m_brick_texture, 32);
	ret->m_brick_texture_view = VWrap::ImageView::Create(device, ret->m_brick_texture);

	ret->WriteDescriptors();

	return ret;
}

void OctreeTracer::CreateDescriptors(int max_sets)
{
	VkDescriptorSetLayoutBinding sampled_image_binding{};
	sampled_image_binding.binding = 0;
	sampled_image_binding.descriptorCount = 1;
	sampled_image_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sampled_image_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	std::vector<VkDescriptorSetLayoutBinding> bindings = { sampled_image_binding };
	m_descriptor_set_layout = VWrap::DescriptorSetLayout::Create(m_device, bindings);

	std::vector<VkDescriptorPoolSize> poolSizes(1);
	poolSizes[0].descriptorCount = max_sets;
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	m_descriptor_pool = VWrap::DescriptorPool::Create(m_device, poolSizes, max_sets, 0);

	std::vector<std::shared_ptr<VWrap::DescriptorSetLayout>> layouts(static_cast<size_t>(max_sets), m_descriptor_set_layout);
	m_descriptor_sets = VWrap::DescriptorSet::CreateMany(m_descriptor_pool, layouts);
}

void OctreeTracer::CreatePipeline(std::shared_ptr<VWrap::RenderPass> render_pass)
{
	auto vert_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_tracer.vert.spv");
	auto frag_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_tracer.frag.spv");


	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexAttributeDescriptionCount = 0;
	vertexInputInfo.vertexBindingDescriptionCount = 0;
	vertexInputInfo.pVertexAttributeDescriptions = nullptr;
	vertexInputInfo.pVertexBindingDescriptions = nullptr;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

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
	rasterizer.depthBiasConstantFactor = 0.0f; // Optional
	rasterizer.depthBiasClamp = 0.0f; // Optional
	rasterizer.depthBiasSlopeFactor = 0.0f; // Optional

	VkPushConstantRange pushConstantRange = {};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; // Specify the shader stages that will use the push constants
	pushConstantRange.offset = 0; // Offset of the push constants in bytes
	pushConstantRange.size = sizeof(VWrap::PushConstantBlock); // Size of the push constant block
	std::vector<VkPushConstantRange> push_constant_ranges = { pushConstantRange };

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f; // Optional
	depthStencil.maxDepthBounds = 1.0f; // Optional
	depthStencil.stencilTestEnable = VK_FALSE;
	depthStencil.front = {}; // Optional
	depthStencil.back = {}; // Optional

	VWrap::PipelineCreateInfo create_info{};
	create_info.extent = m_extent;
	create_info.render_pass = render_pass;
	create_info.descriptor_set_layout = m_descriptor_set_layout;
	create_info.vertex_input_info = vertexInputInfo;
	create_info.input_assembly = inputAssembly;
	create_info.dynamic_state = dynamicState;
	create_info.rasterizer = rasterizer;
	create_info.depth_stencil = depthStencil;
	create_info.push_constant_ranges = push_constant_ranges;
	create_info.subpass = 0;

	m_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_shader_code, frag_shader_code);
}

void OctreeTracer::WriteDescriptors()
{
	for (size_t i = 0; i < m_descriptor_sets.size(); i++) {

		VkDescriptorImageInfo image_info{};
		image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_info.imageView = m_brick_texture_view->Get();
		image_info.sampler = m_sampler->Get();

		// array of descriptor writes:
		std::array<VkWriteDescriptorSet, 1> descriptorWrites{};

		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].dstBinding = 0;
		descriptorWrites[0].dstSet = m_descriptor_sets[i]->Get();
		descriptorWrites[0].dstArrayElement = 0;
		descriptorWrites[0].pImageInfo = &image_info;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

		vkUpdateDescriptorSets(m_device->Get(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
	}
}

void OctreeTracer::CmdDraw(std::shared_ptr<VWrap::CommandBuffer> command_buffer, uint32_t frame, std::shared_ptr<Camera> camera)
{
	auto vk_command_buffer = command_buffer->Get();
	vkCmdBindPipeline(vk_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->Get());

	std::array<VkDescriptorSet, 1> descriptorSets = { m_descriptor_sets[frame]->Get() };
	vkCmdBindDescriptorSets(vk_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->GetLayout(), 0, 1, descriptorSets.data(), 0, nullptr);


	VkViewport viewport{};
	viewport.height = (float)m_extent.height;
	viewport.width = (float)m_extent.width;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	viewport.x = 0;
	viewport.y = 0;

	vkCmdSetViewport(vk_command_buffer, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = m_extent;
	scissor.offset = { 0,0 };

	vkCmdSetScissor(vk_command_buffer, 0, 1, &scissor);

	VWrap::PushConstantBlock PCB;
	PCB.NDCtoWorld = camera->GetNDCtoWorldMatrix();
	PCB.cameraPos = camera->GetPosition();

	// Populate pushConstants with the necessary data
	vkCmdPushConstants(
		vk_command_buffer,
		m_pipeline->GetLayout(), // The pipeline layout used for the push constants
		VK_SHADER_STAGE_FRAGMENT_BIT, // Shader stage the push constants will be used in
		0, // Offset of the push constants to update
		sizeof(VWrap::PushConstantBlock), // Size of the push constants to update
		&PCB // Pointer to the data to copy
	);

	vkCmdDraw(vk_command_buffer, 4, 1, 0, 0);
}
