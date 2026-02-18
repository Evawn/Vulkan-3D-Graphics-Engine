#include "SVORenderer.h"
#include "PipelineDefaults.h"
#include "config.h"
#include <spdlog/spdlog.h>

struct SVOGeneratePC {
	int shape;
	float time;
};

struct SVOTracePC {
	glm::mat4 NDCtoWorld;
	glm::vec3 cameraPos;
	int maxIterations;
	glm::vec3 skyColor;
	int debugColor;
};

void SVORenderer::RegisterPasses(
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
	m_camera = ctx.camera;
	m_start_time = std::chrono::steady_clock::now();

	// 64^3 3D storage image for voxel volume
	m_volume = graph.CreateImage("svo_volume", {
		64, 64, 64,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_TYPE_3D
	});

	// Compute pass: generate voxels into volume
	graph.AddComputePass("SVO Generate")
		.Write(m_volume)
		.SetRecord([this](PassContext& ctx) {
			ctx.cmd->CmdBindComputePipeline(m_compute_pipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(m_compute_pipeline->GetLayout(),
				{ m_compute_descriptor_set->Get() });
			SVOGeneratePC pc{};
			pc.shape = m_shape;
			auto now = std::chrono::steady_clock::now();
			pc.time = std::chrono::duration<float>(now - m_start_time).count() * m_time_scale;
			vkCmdPushConstants(ctx.cmd->Get(), m_compute_pipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			// 64 / 4 = 16 workgroups per axis
			ctx.cmd->CmdDispatch(16, 16, 16);
		});

	// Graphics pass: DDA ray-march the volume
	auto& gfx = graph.AddGraphicsPass("SVO Trace")
		.SetColorAttachment(colorTarget, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(depthTarget, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(resolveTarget)
		.Read(m_volume)
		.SetRecord([this](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline->Get());
			VkDescriptorSet ds = m_graphics_descriptor_sets[ctx.frameIndex]->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_graphics_pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			SVOTracePC pc{};
			pc.NDCtoWorld = m_camera->GetNDCtoWorldMatrix();
			pc.cameraPos = m_camera->GetPosition();
			pc.maxIterations = m_max_iterations;
			pc.skyColor = glm::vec3(m_sky_color[0], m_sky_color[1], m_sky_color[2]);
			pc.debugColor = m_debug_color ? 1 : 0;
			vkCmdPushConstants(vk_cmd, m_graphics_pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SVOTracePC), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		});

	// Compute descriptors (1 storage image)
	logger->debug("SVORenderer: Creating compute descriptors...");
	auto computeDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.Build(1);
	m_compute_descriptor_layout = computeDesc.layout;
	m_compute_descriptor_pool = computeDesc.pool;
	m_compute_descriptor_set = computeDesc.sets[0];

	logger->debug("SVORenderer: Creating compute pipeline...");
	CreateComputePipeline();

	// Graphics descriptors (per-frame combined image sampler)
	logger->debug("SVORenderer: Creating graphics descriptors...");
	auto gfxDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(ctx.maxFramesInFlight);
	m_graphics_descriptor_layout = gfxDesc.layout;
	m_graphics_descriptor_pool = gfxDesc.pool;
	m_graphics_descriptor_sets = gfxDesc.sets;

	m_render_pass = gfx.GetRenderPassPtr();

	logger->debug("SVORenderer: Creating graphics pipeline...");
	CreateGraphicsPipeline();

	logger->debug("SVORenderer: Creating sampler...");
	m_sampler = VWrap::Sampler::Create(m_device);

	logger->debug("SVORenderer: Initialized via RegisterPasses");
}

void SVORenderer::WriteGraphDescriptors(RenderGraph& graph) {
	auto volumeView = graph.GetImageView(m_volume);

	// Compute descriptor: storage image in GENERAL layout
	VkDescriptorImageInfo computeImageInfo{};
	computeImageInfo.imageView = volumeView->Get();
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
		imageInfo.imageView = volumeView->Get();
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

void SVORenderer::CreateComputePipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/svo_generate.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(SVOGeneratePC);

	m_compute_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_compute_descriptor_layout, { pushRange }, comp_code);
}

void SVORenderer::CreateGraphicsPipeline() {
	auto vert_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/svo_trace.vert.spv");
	auto frag_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/svo_trace.frag.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(SVOTracePC);

	auto create_info = PipelineDefaults::FullscreenQuad(
		m_render_pass, m_graphics_descriptor_layout, m_extent, { pushRange });

	m_graphics_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_code, frag_code);
}

void SVORenderer::OnResize(VkExtent2D newExtent, RenderGraph& graph) {
	m_extent = newExtent;
	WriteGraphDescriptors(graph);
}

std::vector<std::string> SVORenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/svo_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/svo_trace.vert.spv",
		std::string(config::SHADER_DIR) + "/svo_trace.frag.spv"
	};
}

void SVORenderer::RecreatePipeline(const RenderContext& ctx) {
	m_graphics_pipeline.reset();
	m_compute_pipeline.reset();
	CreateComputePipeline();
	CreateGraphicsPipeline();
}

std::vector<TechniqueParameter>& SVORenderer::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Shape", TechniqueParameter::Enum, &m_shape, 0.0f, 0.0f,
				{ "Sphere", "Torus", "Box Frame", "Cylinder", "Cone", "Octahedron", "Gyroid", "Sine Blob", "Menger Sponge" } },
			{ "Time Scale", TechniqueParameter::Float, &m_time_scale, 0.0f, 5.0f },
			{ "Max Iterations", TechniqueParameter::Int, &m_max_iterations, 1.0f, 500.0f },
			{ "Sky Color", TechniqueParameter::Color3, m_sky_color },
			{ "Debug Coloring", TechniqueParameter::Bool, &m_debug_color },
		};
	}
	return m_parameters;
}

FrameStats SVORenderer::GetFrameStats() const {
	return { 2, 4, 0 };
}
