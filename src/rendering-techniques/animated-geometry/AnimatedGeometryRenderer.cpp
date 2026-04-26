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

RenderTargetDesc AnimatedGeometryRenderer::DescribeTargets(const RendererCaps& caps) const {
	RenderTargetDesc desc{};
	desc.color.format       = caps.swapchainFormat;
	desc.color.samples      = caps.msaaSamples;
	desc.color.needsResolve = (caps.msaaSamples != VK_SAMPLE_COUNT_1_BIT);
	desc.hasDepth     = true;
	desc.depthFormat  = caps.depthFormat;
	desc.depthSamples = caps.msaaSamples;
	return desc;
}

void AnimatedGeometryRenderer::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	const TechniqueTargets& targets)
{
	auto logger = spdlog::get("Render");
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_graphics_pool = ctx.graphicsPool;
	m_camera = ctx.camera;
	m_lighting = ctx.lighting;
	m_start_time = std::chrono::steady_clock::now();

	glm::uvec3 vs = m_volume_size;
	logger->info("AnimatedGeometryRenderer: volume={}x{}x{}", vs.x, vs.y, vs.z);

	m_volume = graph.CreateImage("animated_geometry_volume", {
		vs.x, vs.y, vs.z,
		VK_FORMAT_R8_UINT,
		VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_TYPE_3D,
		0
	});

	// Palette + volume sampler: external resources the technique owns. Built
	// here so binding-table sources can capture them.
	m_palette = std::make_unique<PaletteResource>(m_device, m_allocator, m_graphics_pool);
	m_palette->Create();
	if (!m_volume_sampler) {
		m_volume_sampler = VWrap::Sampler::CreateNearestClamp(m_device);
	}

	// Compute binding table: storage image (volume).
	m_compute_bindings = std::make_shared<BindingTable>(m_device, 1);
	m_compute_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.BindGraphStorageImage(0, m_volume);
	m_compute_bindings->Build();

	// Graphics binding table: volume sampled (graph-managed) + palette sampled (external).
	m_graphics_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_graphics_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.BindGraphSampledImage(0, m_volume, m_volume_sampler)
		.BindExternalSampledImage(1, m_palette->GetImageView(), m_palette->GetSampler(),
		                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_graphics_bindings->Build();

	// Compute pass: write procedural animated voxels into the volume each frame.
	// Tagged AsyncCompute so the dispatch overlaps with the previous frame's
	// graphics tail (post-process / UI) on devices with a dedicated compute
	// queue family. The graph emits the queue-family-ownership transfer barriers
	// for m_volume and a binary semaphore signal-wait between the async submit
	// and the graphics submit. On single-queue-family devices the hint is
	// silently demoted (logged once) and the pass runs on the graphics queue.
	graph.AddComputePass("Animated Geometry Generate")
		.SetQueueAffinity(QueueAffinity::AsyncCompute)
		.Write(m_volume)
		.SetPipeline([this]() {
			ComputePipelineDesc d;
			d.compSpvPath = std::string(config::SHADER_DIR) + "/animated_geometry_generate.comp.spv";
			d.descriptorSetLayout = m_compute_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			r.offset = 0;
			r.size = sizeof(AnimatedGeometryGeneratePC);
			d.pushConstantRanges = { r };
			return d;
		})
		.SetRecord([this, vs](PassContext& ctx) {
			ctx.cmd->CmdBindComputePipeline(ctx.computePipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(ctx.computePipeline->GetLayout(),
				{ m_compute_bindings->GetSet(0)->Get() });
			AnimatedGeometryGeneratePC pc{};
			pc.pattern = m_pattern;
			auto now = std::chrono::steady_clock::now();
			pc.time = std::chrono::duration<float>(now - m_start_time).count() * m_time_scale;
			pc.volume_size_x = static_cast<int>(vs.x);
			pc.volume_size_y = static_cast<int>(vs.y);
			pc.volume_size_z = static_cast<int>(vs.z);
			vkCmdPushConstants(ctx.cmd->Get(), ctx.computePipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			ctx.cmd->CmdDispatch(cdiv(vs.x, 4), cdiv(vs.y, 4), cdiv(vs.z, 4));
		})
		.SetBindings(m_compute_bindings);

	// Graphics pass: single-level DDA fragment shader.
	graph.AddGraphicsPass("Animated Geometry Trace")
		.SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(targets.resolve)
		.Read(m_volume, ResourceUsage::SampledRead)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/animated_geometry_trace.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/animated_geometry_trace.frag.spv";
			d.descriptorSetLayout = m_graphics_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			r.offset = 0;
			r.size = sizeof(AnimatedGeometryTracePC);
			d.pushConstantRanges = { r };
			d.inputAssembly = PipelineDefaults::TriangleStrip();
			d.rasterizer = PipelineDefaults::NoCullFill();
			d.depthStencil = PipelineDefaults::NoDepthTest();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this, vs](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.graphicsPipeline->Get());
			VkDescriptorSet ds = m_graphics_bindings->GetSet(ctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				ctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

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
			vkCmdPushConstants(vk_cmd, ctx.graphicsPipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(AnimatedGeometryTracePC), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		})
		.SetBindings(m_graphics_bindings);

	logger->debug("AnimatedGeometryRenderer: Initialized via RegisterPasses");
}

std::vector<std::string> AnimatedGeometryRenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/animated_geometry_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/animated_geometry_trace.vert.spv",
		std::string(config::SHADER_DIR) + "/animated_geometry_trace.frag.spv"
	};
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
