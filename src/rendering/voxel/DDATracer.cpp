#include "DDATracer.h"
#include "config.h"
#include <spdlog/spdlog.h>

struct TracerPushConstants {
	glm::mat4 NDCtoWorld;
	glm::vec3 cameraPos;
	int maxIterations;
	glm::vec3 skyColor;
	int debugColor;
};

void DDATracer::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	ImageHandle colorTarget,
	ImageHandle depthTarget,
	ImageHandle resolveTarget)
{
	auto logger = spdlog::get("Render");
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_extent = ctx.extent;
	m_graphics_pool = ctx.graphicsPool;
	m_camera = ctx.camera;

	logger->debug("DDATracer: Creating descriptors...");
	CreateDescriptors(ctx.maxFramesInFlight);

	logger->debug("DDATracer: Creating sampler...");
	m_sampler = VWrap::Sampler::Create(m_device);

	logger->debug("DDATracer: Creating brick texture...");
	VWrap::CommandBuffer::CreateAndFillBrickTexture(m_graphics_pool, m_allocator, m_brick_texture, 32);
	m_brick_texture_view = VWrap::ImageView::Create(m_device, m_brick_texture);

	logger->debug("DDATracer: Writing descriptors...");
	WriteDescriptors();

	auto& pass = graph.AddGraphicsPass("DDA Scene")
		.SetColorAttachment(colorTarget, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(depthTarget, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(resolveTarget)
		.SetRecord([this](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->Get());

			std::array<VkDescriptorSet, 1> descriptorSets = { m_descriptor_sets[ctx.frameIndex]->Get() };
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->GetLayout(), 0, 1, descriptorSets.data(), 0, nullptr);

			TracerPushConstants pc{};
			pc.NDCtoWorld = m_camera->GetNDCtoWorldMatrix();
			pc.cameraPos = m_camera->GetPosition();
			pc.maxIterations = m_max_iterations;
			pc.skyColor = glm::vec3(m_sky_color[0], m_sky_color[1], m_sky_color[2]);
			pc.debugColor = m_debug_color ? 1 : 0;
			vkCmdPushConstants(vk_cmd, m_pipeline->GetLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TracerPushConstants), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		});

	m_render_pass = pass.GetRenderPassPtr();

	logger->debug("DDATracer: Creating pipeline...");
	CreatePipeline(m_render_pass);
	logger->debug("DDATracer: Initialized via RegisterPasses");
}

void DDATracer::CreateDescriptors(int max_sets)
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

void DDATracer::CreatePipeline(std::shared_ptr<VWrap::RenderPass> render_pass)
{
	auto vert_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_dda.vert.spv");
	auto frag_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_dda.frag.spv");

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexAttributeDescriptionCount = 0;
	vertexInputInfo.vertexBindingDescriptionCount = 0;
	vertexInputInfo.pVertexAttributeDescriptions = nullptr;
	vertexInputInfo.pVertexBindingDescriptions = nullptr;

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

	VkPushConstantRange pushConstantRange = {};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(TracerPushConstants);
	std::vector<VkPushConstantRange> push_constant_ranges = { pushConstantRange };

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

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

void DDATracer::WriteDescriptors()
{
	for (size_t i = 0; i < m_descriptor_sets.size(); i++) {
		VkDescriptorImageInfo image_info{};
		image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_info.imageView = m_brick_texture_view->Get();
		image_info.sampler = m_sampler->Get();

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

std::vector<std::string> DDATracer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/shader_dda.vert.spv",
		std::string(config::SHADER_DIR) + "/shader_dda.frag.spv"
	};
}

void DDATracer::RecreatePipeline(const RenderContext& ctx) {
	m_pipeline.reset();
	CreatePipeline(m_render_pass);
}

std::vector<TechniqueParameter>& DDATracer::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Max Iterations", TechniqueParameter::Int, &m_max_iterations, 1.0f, 500.0f },
			{ "Sky Color", TechniqueParameter::Color3, m_sky_color },
			{ "Debug Coloring", TechniqueParameter::Bool, &m_debug_color },
		};
	}
	return m_parameters;
}

FrameStats DDATracer::GetFrameStats() const {
	return { 1, 4, 0 };
}
