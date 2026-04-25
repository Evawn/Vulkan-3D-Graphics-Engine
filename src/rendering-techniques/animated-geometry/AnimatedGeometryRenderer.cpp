#include "AnimatedGeometryRenderer.h"
#include "PipelineDefaults.h"
#include "config.h"
#include <spdlog/spdlog.h>
#include <cstring>

struct AnimatedGeometryGeneratePC {
	int pattern;
	float time;
	int volume_size_x;
	int volume_size_y;
	int volume_size_z;
};

// Layout matches shaders/animated_geometry_trace.frag — std140 rules: vec3
// followed by a scalar packs into 16 bytes; mat4 starts 16-byte aligned.
struct AnimatedGeometryTracePC {
	glm::mat4 NDCtoWorld;
	glm::vec3 cameraPos;      int maxIterations;
	glm::vec3 skyColor;       int debugColor;
	glm::vec3 sunDirection;   float sunCosHalfAngle;
	glm::vec3 sunColor;       float sunIntensity;
	glm::ivec3 volumeSize;    float ambientIntensity;
	float aoStrength;         int shadowsEnabled;
	int   _pad0;              int   _pad1;
};
static_assert(sizeof(AnimatedGeometryTracePC) == 160,
	"AnimatedGeometryTracePC must stay in std140 layout — 160 bytes");

void AnimatedGeometryRenderer::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	ImageHandle colorTarget,
	ImageHandle depthTarget,
	ImageHandle resolveTarget)
{
	auto logger = spdlog::get("Render");
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_graphics_pool = ctx.graphicsPool;
	m_extent = ctx.extent;
	m_camera = ctx.camera;
	m_lighting = ctx.lighting;
	m_start_time = std::chrono::steady_clock::now();

	glm::uvec3 vs = m_volume_size;
	logger->info("AnimatedGeometryRenderer: volume={}x{}x{}", vs.x, vs.y, vs.z);

	// 3D storage image for voxel volume (R8_UINT: material index per voxel).
	// Both compute (write) and graphics (sampled read) bits are added by the
	// graph automatically based on pass declarations.
	m_volume = graph.CreateImage("animated_geometry_volume", {
		vs.x, vs.y, vs.z,
		VK_FORMAT_R8_UINT,
		VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_TYPE_3D,
		0
	});

	// Compute pass: write procedural animated voxels into the volume each frame.
	graph.AddComputePass("Animated Geometry Generate")
		.Write(m_volume)
		.SetRecord([this, vs](PassContext& ctx) {
			ctx.cmd->CmdBindComputePipeline(m_compute_pipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(m_compute_pipeline->GetLayout(),
				{ m_compute_descriptor_set->Get() });
			AnimatedGeometryGeneratePC pc{};
			pc.pattern = m_pattern;
			auto now = std::chrono::steady_clock::now();
			pc.time = std::chrono::duration<float>(now - m_start_time).count() * m_time_scale;
			pc.volume_size_x = static_cast<int>(vs.x);
			pc.volume_size_y = static_cast<int>(vs.y);
			pc.volume_size_z = static_cast<int>(vs.z);
			vkCmdPushConstants(ctx.cmd->Get(), m_compute_pipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			ctx.cmd->CmdDispatch(cdiv(vs.x, 4), cdiv(vs.y, 4), cdiv(vs.z, 4));
		});

	// Graphics pass: single-level DDA fragment shader.
	auto& gfx = graph.AddGraphicsPass("Animated Geometry Trace")
		.SetColorAttachment(colorTarget, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(depthTarget, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(resolveTarget)
		.Read(m_volume)
		.SetRecord([this, vs](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline->Get());
			VkDescriptorSet ds = m_graphics_descriptor_sets[ctx.frameIndex]->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_graphics_pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			AnimatedGeometryTracePC pc{};
			pc.NDCtoWorld = m_camera->GetNDCtoWorldMatrix();
			pc.cameraPos = m_camera->GetPosition();
			pc.maxIterations = m_max_iterations;
			pc.skyColor = glm::vec3(m_sky_color[0], m_sky_color[1], m_sky_color[2]);
			pc.debugColor = m_debug_color ? 1 : 0;
			pc.volumeSize = glm::ivec3(vs);
			if (m_lighting) {
				pc.sunDirection = m_lighting->GetSunDirection();
				pc.sunCosHalfAngle = m_lighting->GetSunCosHalfAngle();
				pc.sunColor = glm::vec3(m_lighting->sunColor[0], m_lighting->sunColor[1], m_lighting->sunColor[2]);
				pc.sunIntensity = m_lighting->sunIntensity;
				pc.ambientIntensity = m_lighting->ambientIntensity;
				pc.aoStrength = m_lighting->aoStrength;
				pc.shadowsEnabled = m_lighting->shadowsEnabled ? 1 : 0;
			} else {
				pc.sunDirection = glm::vec3(0.0f, 0.0f, -1.0f);
				pc.sunCosHalfAngle = 1.0f;
				pc.sunColor = glm::vec3(1.0f);
				pc.sunIntensity = 0.0f;
				pc.ambientIntensity = 1.0f;
				pc.aoStrength = 0.0f;
				pc.shadowsEnabled = 0;
			}
			pc._pad0 = 0;
			pc._pad1 = 0;
			vkCmdPushConstants(vk_cmd, m_graphics_pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(AnimatedGeometryTracePC), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		});

	// Generate descriptor: one storage image for write.
	auto computeDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.Build(1);
	m_compute_descriptor_layout = computeDesc.layout;
	m_compute_descriptor_pool = computeDesc.pool;
	m_compute_descriptor_set = computeDesc.sets[0];

	CreateComputePipeline();

	// Graphics descriptors: volume sampler + palette sampler per frame.
	auto gfxDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(ctx.maxFramesInFlight);
	m_graphics_descriptor_layout = gfxDesc.layout;
	m_graphics_descriptor_pool = gfxDesc.pool;
	m_graphics_descriptor_sets = gfxDesc.sets;

	m_render_pass = gfx.GetRenderPassPtr();

	CreateGraphicsPipeline();

	m_palette = std::make_unique<PaletteResource>(m_device, m_allocator, m_graphics_pool);
	m_palette->Create();

	// NEAREST sampler for the integer R8_UINT volume.
	if (!m_volume_sampler) {
		m_volume_sampler = VWrap::Sampler::CreateNearestClamp(m_device);
	}

	logger->debug("AnimatedGeometryRenderer: Initialized via RegisterPasses");
}

void AnimatedGeometryRenderer::WriteGraphDescriptors(RenderGraph& graph) {
	auto volumeView = graph.GetImageView(m_volume);

	// Generate: storage image in GENERAL layout.
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

	// Graphics: volume (sampled) + palette per frame.
	for (auto& ds : m_graphics_descriptor_sets) {
		VkDescriptorImageInfo volumeInfo{};
		volumeInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		volumeInfo.imageView = volumeView->Get();
		volumeInfo.sampler = m_volume_sampler->Get();

		VkDescriptorImageInfo paletteInfo{};
		paletteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		paletteInfo.imageView = m_palette->GetImageView()->Get();
		paletteInfo.sampler = m_palette->GetSampler()->Get();

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = ds->Get();
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &volumeInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = ds->Get();
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[1].pImageInfo = &paletteInfo;

		vkUpdateDescriptorSets(m_device->Get(), 2, writes, 0, nullptr);
	}
}

void AnimatedGeometryRenderer::CreateComputePipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/animated_geometry_generate.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(AnimatedGeometryGeneratePC);

	m_compute_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_compute_descriptor_layout, { pushRange }, comp_code);
}

void AnimatedGeometryRenderer::CreateGraphicsPipeline() {
	auto vert_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/animated_geometry_trace.vert.spv");
	auto frag_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/animated_geometry_trace.frag.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(AnimatedGeometryTracePC);

	auto create_info = PipelineDefaults::FullscreenQuad(
		m_render_pass, m_graphics_descriptor_layout, m_extent, { pushRange });

	m_graphics_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_code, frag_code);
}

void AnimatedGeometryRenderer::OnResize(VkExtent2D newExtent, RenderGraph& graph) {
	m_extent = newExtent;
	WriteGraphDescriptors(graph);
}

std::vector<std::string> AnimatedGeometryRenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/animated_geometry_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/animated_geometry_trace.vert.spv",
		std::string(config::SHADER_DIR) + "/animated_geometry_trace.frag.spv"
	};
}

void AnimatedGeometryRenderer::RecreatePipeline(const RenderContext& ctx) {
	m_graphics_pipeline.reset();
	m_compute_pipeline.reset();
	CreateComputePipeline();
	CreateGraphicsPipeline();
}

std::vector<TechniqueParameter>& AnimatedGeometryRenderer::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Pattern", TechniqueParameter::Enum, &m_pattern, 0.0f, 0.0f,
				{ "Wobble Sphere", "Plasma", "Wave Field" } },
			{ "Time Scale", TechniqueParameter::Float, &m_time_scale, 0.0f, 5.0f },
			{ "Max Iterations", TechniqueParameter::Int, &m_max_iterations, 1.0f, 500.0f },
			{ "Sky Color", TechniqueParameter::Color3, m_sky_color },
			{ "Debug Coloring", TechniqueParameter::Bool, &m_debug_color },
		};
	}
	return m_parameters;
}

FrameStats AnimatedGeometryRenderer::GetFrameStats() const {
	return { 2, 4, 0 };
}
