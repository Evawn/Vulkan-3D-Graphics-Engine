#include "DDATracer.h"
#include "PipelineDefaults.h"
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
	auto desc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(ctx.maxFramesInFlight);
	m_descriptor_set_layout = desc.layout;
	m_descriptor_pool = desc.pool;
	m_descriptor_sets = desc.sets;

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

void DDATracer::CreatePipeline(std::shared_ptr<VWrap::RenderPass> render_pass)
{
	auto vert_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_dda.vert.spv");
	auto frag_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_dda.frag.spv");

	VkPushConstantRange pushConstantRange = {};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(TracerPushConstants);

	auto create_info = PipelineDefaults::FullscreenQuad(
		render_pass, m_descriptor_set_layout, m_extent, { pushConstantRange });

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
