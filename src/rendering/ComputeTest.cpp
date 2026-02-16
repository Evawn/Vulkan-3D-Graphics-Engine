#include "ComputeTest.h"
#include "config.h"
#include <spdlog/spdlog.h>

struct ComputeTestPC {
	float width;
	float height;
};

void ComputeTest::RegisterPasses(
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

	// Transient storage image — graph allocates it
	m_storage = graph.CreateImage("compute_output", {
		ctx.extent.width, ctx.extent.height, 1,
		VK_FORMAT_R8G8B8A8_UNORM
	});

	// Compute pass: write to storage image
	graph.AddComputePass("Compute Generate")
		.Write(m_storage)
		.SetRecord([this](PassContext& ctx) {
			ctx.cmd->CmdBindComputePipeline(m_compute_pipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(m_compute_pipeline->GetLayout(),
				{ m_compute_descriptor_set->Get() });
			ComputeTestPC pc{};
			pc.width = static_cast<float>(ctx.extent.width);
			pc.height = static_cast<float>(ctx.extent.height);
			vkCmdPushConstants(ctx.cmd->Get(), m_compute_pipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			uint32_t gx = (ctx.extent.width + 15) / 16;
			uint32_t gy = (ctx.extent.height + 15) / 16;
			ctx.cmd->CmdDispatch(gx, gy, 1);
		});

	// Graphics pass: display the compute result
	auto& gfx = graph.AddGraphicsPass("Compute Display")
		.SetColorAttachment(colorTarget, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(depthTarget, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(resolveTarget)
		.Read(m_storage)
		.SetRecord([this](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline->Get());
			VkDescriptorSet ds = m_graphics_descriptor_sets[ctx.frameIndex]->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_graphics_pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		});

	// Create pipelines and descriptors
	logger->debug("ComputeTest: Creating compute descriptors...");
	CreateComputeDescriptors();

	logger->debug("ComputeTest: Creating compute pipeline...");
	CreateComputePipeline();

	logger->debug("ComputeTest: Creating graphics descriptors...");
	CreateGraphicsDescriptors(ctx.maxFramesInFlight);

	m_render_pass = gfx.GetRenderPassPtr();

	logger->debug("ComputeTest: Creating graphics pipeline...");
	CreateGraphicsPipeline();

	logger->debug("ComputeTest: Creating sampler...");
	m_sampler = VWrap::Sampler::Create(m_device);

	// Note: descriptors referencing the storage image are written in WriteGraphDescriptors()
	// after graph.Compile() allocates the actual image.
	logger->debug("ComputeTest: Initialized via RegisterPasses");
}

void ComputeTest::WriteGraphDescriptors(RenderGraph& graph) {
	auto storageView = graph.GetImageView(m_storage);

	// Compute descriptor: storage image in GENERAL layout
	VkDescriptorImageInfo computeImageInfo{};
	computeImageInfo.imageView = storageView->Get();
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
		imageInfo.imageView = storageView->Get();
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

void ComputeTest::OnResize(VkExtent2D newExtent, RenderGraph& graph) {
	m_extent = newExtent;
	// The graph handles storage image reallocation via Resize()
	// We just need to update descriptors after the graph resizes
	WriteGraphDescriptors(graph);
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
}

FrameStats ComputeTest::GetFrameStats() const {
	return { 1, 4, 0 };
}
