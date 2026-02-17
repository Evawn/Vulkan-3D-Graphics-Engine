#include "ComputeTest.h"
#include "PipelineDefaults.h"
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

	// Compute descriptors (1 storage image)
	logger->debug("ComputeTest: Creating compute descriptors...");
	auto computeDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.Build(1);
	m_compute_descriptor_layout = computeDesc.layout;
	m_compute_descriptor_pool = computeDesc.pool;
	m_compute_descriptor_set = computeDesc.sets[0];

	logger->debug("ComputeTest: Creating compute pipeline...");
	CreateComputePipeline();

	// Graphics descriptors (per-frame sampled image)
	logger->debug("ComputeTest: Creating graphics descriptors...");
	auto gfxDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(ctx.maxFramesInFlight);
	m_graphics_descriptor_layout = gfxDesc.layout;
	m_graphics_descriptor_pool = gfxDesc.pool;
	m_graphics_descriptor_sets = gfxDesc.sets;

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

void ComputeTest::CreateComputePipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/compute_test.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(ComputeTestPC);

	m_compute_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_compute_descriptor_layout, { pushRange }, comp_code);
}

void ComputeTest::CreateGraphicsPipeline() {
	auto vert_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/compute_test.vert.spv");
	auto frag_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/compute_test.frag.spv");

	auto create_info = PipelineDefaults::FullscreenQuad(
		m_render_pass, m_graphics_descriptor_layout, m_extent);

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
