#include "BrickmapRenderer.h"
#include "PipelineDefaults.h"
#include "config.h"
#include <spdlog/spdlog.h>

struct BrickmapGeneratePC {
	int shape;
	float time;
};

struct BrickmapBuildPC {
	int volume_size;
	int brick_size;
};

struct BrickmapTracePC {
	glm::mat4 NDCtoWorld;
	glm::vec3 cameraPos;
	int maxIterations;
	glm::vec3 skyColor;
	int debugColor;
};

void BrickmapRenderer::RegisterPasses(
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

	// 128^3 3D storage image for voxel volume
	m_volume = graph.CreateImage("brickmap_volume", {
		128, 128, 128,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_TYPE_3D
	});

	// Brickmap structure buffer:
	//   Header:       16 bytes (4 uint32)
	//   Top grid:     16^3 * 4 = 16384 bytes
	//   Brick pool:   up to 4096 bricks * 16 uint32 * 4 = 262144 bytes
	//   Total:        278544 bytes
	m_brickmap_buffer = graph.CreateBuffer("brickmap_data", {
		278544,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
	});

	// Compute pass: generate voxels into volume
	graph.AddComputePass("Brickmap Generate")
		.Write(m_volume)
		.SetRecord([this](PassContext& ctx) {
			ctx.cmd->CmdBindComputePipeline(m_compute_pipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(m_compute_pipeline->GetLayout(),
				{ m_compute_descriptor_set->Get() });
			BrickmapGeneratePC pc{};
			pc.shape = m_shape;
			auto now = std::chrono::steady_clock::now();
			pc.time = std::chrono::duration<float>(now - m_start_time).count() * m_time_scale;
			vkCmdPushConstants(ctx.cmd->Get(), m_compute_pipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			// 128 / 4 = 32 workgroups per axis
			ctx.cmd->CmdDispatch(32, 32, 32);
		});

	// Compute pass: build brickmap from voxel volume (parallel, one thread per grid cell)
	graph.AddComputePass("Brickmap Build")
		.Read(m_volume)
		.Write(m_brickmap_buffer)
		.SetRecord([this](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();

			// Zero the brick_count atomic counter (offset 12, size 4) before parallel dispatch
			vkCmdFillBuffer(vk_cmd, m_brickmap_vk_buffer, 12, 4, 0);

			VkMemoryBarrier memBarrier{};
			memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(vk_cmd,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 1, &memBarrier, 0, nullptr, 0, nullptr);

			ctx.cmd->CmdBindComputePipeline(m_build_pipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(m_build_pipeline->GetLayout(),
				{ m_build_descriptor_set->Get() });
			BrickmapBuildPC pc{};
			pc.volume_size = 128;
			pc.brick_size = 8;
			vkCmdPushConstants(vk_cmd, m_build_pipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			// 16 / 4 = 4 workgroups per axis (4^3 * 4^3 = 4096 threads = 16^3 grid cells)
			ctx.cmd->CmdDispatch(4, 4, 4);
		});

	// Graphics pass: brickmap two-level DDA ray-march
	auto& gfx = graph.AddGraphicsPass("Brickmap Trace")
		.SetColorAttachment(colorTarget, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(depthTarget, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(resolveTarget)
		.Read(m_volume)
		.Read(m_brickmap_buffer)
		.SetRecord([this](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline->Get());
			VkDescriptorSet ds = m_graphics_descriptor_sets[ctx.frameIndex]->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_graphics_pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			BrickmapTracePC pc{};
			pc.NDCtoWorld = m_camera->GetNDCtoWorldMatrix();
			pc.cameraPos = m_camera->GetPosition();
			pc.maxIterations = m_max_iterations;
			pc.skyColor = glm::vec3(m_sky_color[0], m_sky_color[1], m_sky_color[2]);
			pc.debugColor = m_debug_color ? 1 : 0;
			vkCmdPushConstants(vk_cmd, m_graphics_pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BrickmapTracePC), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		});

	// Generate descriptors (1 storage image)
	logger->debug("BrickmapRenderer: Creating generate descriptors...");
	auto computeDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.Build(1);
	m_compute_descriptor_layout = computeDesc.layout;
	m_compute_descriptor_pool = computeDesc.pool;
	m_compute_descriptor_set = computeDesc.sets[0];

	logger->debug("BrickmapRenderer: Creating generate pipeline...");
	CreateComputePipeline();

	// Build descriptors (storage image + storage buffer)
	logger->debug("BrickmapRenderer: Creating build descriptors...");
	auto buildDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		.Build(1);
	m_build_descriptor_layout = buildDesc.layout;
	m_build_descriptor_pool = buildDesc.pool;
	m_build_descriptor_set = buildDesc.sets[0];

	logger->debug("BrickmapRenderer: Creating build pipeline...");
	CreateBuildPipeline();

	// Graphics descriptors (per-frame: combined image sampler + brickmap buffer)
	logger->debug("BrickmapRenderer: Creating graphics descriptors...");
	auto gfxDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(ctx.maxFramesInFlight);
	m_graphics_descriptor_layout = gfxDesc.layout;
	m_graphics_descriptor_pool = gfxDesc.pool;
	m_graphics_descriptor_sets = gfxDesc.sets;

	m_render_pass = gfx.GetRenderPassPtr();

	logger->debug("BrickmapRenderer: Creating graphics pipeline...");
	CreateGraphicsPipeline();

	logger->debug("BrickmapRenderer: Creating sampler...");
	m_sampler = VWrap::Sampler::Create(m_device);

	logger->debug("BrickmapRenderer: Initialized via RegisterPasses");
}

void BrickmapRenderer::WriteGraphDescriptors(RenderGraph& graph) {
	auto volumeView = graph.GetImageView(m_volume);
	auto brickmapBuffer = graph.GetBuffer(m_brickmap_buffer);
	m_brickmap_vk_buffer = brickmapBuffer->Get();

	// Generate descriptor: storage image in GENERAL layout
	{
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageView = volumeView->Get();
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = m_compute_descriptor_set->Get();
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		write.pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(m_device->Get(), 1, &write, 0, nullptr);
	}

	// Build descriptors: storage image (read) + storage buffer (write)
	{
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageView = volumeView->Get();
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = brickmapBuffer->Get();
		bufferInfo.offset = 0;
		bufferInfo.range = VK_WHOLE_SIZE;

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = m_build_descriptor_set->Get();
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[0].pImageInfo = &imageInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = m_build_descriptor_set->Get();
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(m_device->Get(), 2, writes, 0, nullptr);
	}

	// Graphics descriptors: sampled image + brickmap buffer per frame
	for (auto& ds : m_graphics_descriptor_sets) {
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = volumeView->Get();
		imageInfo.sampler = m_sampler->Get();

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = brickmapBuffer->Get();
		bufferInfo.offset = 0;
		bufferInfo.range = VK_WHOLE_SIZE;

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = ds->Get();
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &imageInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = ds->Get();
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(m_device->Get(), 2, writes, 0, nullptr);
	}
}

void BrickmapRenderer::CreateComputePipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_generate.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(BrickmapGeneratePC);

	m_compute_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_compute_descriptor_layout, { pushRange }, comp_code);
}

void BrickmapRenderer::CreateBuildPipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_build.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(BrickmapBuildPC);

	m_build_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_build_descriptor_layout, { pushRange }, comp_code);
}

void BrickmapRenderer::CreateGraphicsPipeline() {
	auto vert_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_trace.vert.spv");
	auto frag_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_trace.frag.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(BrickmapTracePC);

	auto create_info = PipelineDefaults::FullscreenQuad(
		m_render_pass, m_graphics_descriptor_layout, m_extent, { pushRange });

	m_graphics_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_code, frag_code);
}

void BrickmapRenderer::OnResize(VkExtent2D newExtent, RenderGraph& graph) {
	m_extent = newExtent;
	WriteGraphDescriptors(graph);
}

std::vector<std::string> BrickmapRenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/brickmap_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/brickmap_build.comp.spv",
		std::string(config::SHADER_DIR) + "/brickmap_trace.vert.spv",
		std::string(config::SHADER_DIR) + "/brickmap_trace.frag.spv"
	};
}

void BrickmapRenderer::RecreatePipeline(const RenderContext& ctx) {
	m_graphics_pipeline.reset();
	m_compute_pipeline.reset();
	m_build_pipeline.reset();
	CreateComputePipeline();
	CreateBuildPipeline();
	CreateGraphicsPipeline();
}

std::vector<TechniqueParameter>& BrickmapRenderer::GetParameters() {
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

FrameStats BrickmapRenderer::GetFrameStats() const {
	return { 3, 4, 0 };
}
