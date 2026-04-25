#include "post-process/BloomEffect.h"
#include "RenderGraph.h"
#include "config.h"

namespace {

struct BrightPC {
	float threshold;
	float knee;
};

struct BlurPC {
	glm::vec2 direction;
	glm::vec2 texelSize;
	float radius;
	float _pad[3];
};

struct CompositePC {
	float intensity;
};

std::string ShaderPath(const char* name) {
	return std::string(config::SHADER_DIR) + "/" + name;
}

}  // namespace

ImageHandle BloomEffect::RegisterPasses(
	RenderGraph& graph,
	const PostProcessContext& ctx,
	ImageHandle input,
	VkExtent2D extent)
{
	m_input = input;

	if (!m_sampler) {
		m_sampler = VWrap::Sampler::CreateLinearClamp(ctx.device);
	}

	// All intermediates are full-res and in the same format as the scene. The
	// graph auto-derives COLOR_ATTACHMENT + SAMPLED usage from the pass
	// declarations below.
	ImageDesc desc{ extent.width, extent.height, 1, ctx.sceneFormat };
	m_bright = graph.CreateImage("bloom_bright",    desc);
	m_blurH  = graph.CreateImage("bloom_blur_h",    desc);
	m_bloom  = graph.CreateImage("bloom_blur_v",    desc);
	m_output = graph.CreateImage("bloom_composite", desc);

	const float threshold = m_threshold;
	const float knee = m_knee;
	(void)threshold; (void)knee; // silence unused-capture warning if no captures

	// Bright pass: read scene, write bloom_bright with luminance over threshold.
	m_brightPass = FullscreenPass::Build(
		graph, ctx.device, extent, ctx.maxFramesInFlight,
		FullscreenPassDesc{
			"Bloom Bright", m_bright, { m_input }, m_sampler,
			ShaderPath("bloom_bright.frag.spv"), sizeof(BrightPC) },
		[this](PassContext& ctx, VkPipelineLayout layout) {
			BrightPC c{ m_threshold, m_knee };
			ctx.cmd->CmdPushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, &c, sizeof(c));
		});

	// Horizontal blur: bloom_bright → bloom_blur_h.
	m_blurHPass = FullscreenPass::Build(
		graph, ctx.device, extent, ctx.maxFramesInFlight,
		FullscreenPassDesc{
			"Bloom Blur H", m_blurH, { m_bright }, m_sampler,
			ShaderPath("bloom_blur.frag.spv"), sizeof(BlurPC) },
		[this, extent](PassContext& ctx, VkPipelineLayout layout) {
			BlurPC c{};
			c.direction = glm::vec2(1.0f, 0.0f);
			c.texelSize = glm::vec2(1.0f / float(extent.width), 1.0f / float(extent.height));
			c.radius = m_blurRadius;
			ctx.cmd->CmdPushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, &c, sizeof(c));
		});

	// Vertical blur: bloom_blur_h → bloom_blur_v (the final blurred result).
	m_blurVPass = FullscreenPass::Build(
		graph, ctx.device, extent, ctx.maxFramesInFlight,
		FullscreenPassDesc{
			"Bloom Blur V", m_bloom, { m_blurH }, m_sampler,
			ShaderPath("bloom_blur.frag.spv"), sizeof(BlurPC) },
		[this, extent](PassContext& ctx, VkPipelineLayout layout) {
			BlurPC c{};
			c.direction = glm::vec2(0.0f, 1.0f);
			c.texelSize = glm::vec2(1.0f / float(extent.width), 1.0f / float(extent.height));
			c.radius = m_blurRadius;
			ctx.cmd->CmdPushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, &c, sizeof(c));
		});

	// Composite: scene + blurred bloom → final.
	m_compositePass = FullscreenPass::Build(
		graph, ctx.device, extent, ctx.maxFramesInFlight,
		FullscreenPassDesc{
			"Bloom Composite", m_output, { m_input, m_bloom }, m_sampler,
			ShaderPath("bloom_composite.frag.spv"), sizeof(CompositePC) },
		[this](PassContext& ctx, VkPipelineLayout layout) {
			CompositePC c{ m_intensity };
			ctx.cmd->CmdPushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, &c, sizeof(c));
		});

	return m_output;
}

std::vector<TechniqueParameter>& BloomEffect::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Bloom Enabled",   TechniqueParameter::Bool,  &m_enabled },
			{ "Bloom Threshold", TechniqueParameter::Float, &m_threshold,   0.0f, 1.0f },
			{ "Bloom Knee",      TechniqueParameter::Float, &m_knee,        0.0f, 0.5f },
			{ "Bloom Intensity", TechniqueParameter::Float, &m_intensity,   0.0f, 3.0f },
			{ "Bloom Radius",    TechniqueParameter::Float, &m_blurRadius,  0.5f, 6.0f },
		};
	}
	return m_parameters;
}

std::vector<std::string> BloomEffect::GetShaderPaths() const {
	return {
		ShaderPath("post_fullscreen.vert.spv"),
		ShaderPath("bloom_bright.frag.spv"),
		ShaderPath("bloom_blur.frag.spv"),
		ShaderPath("bloom_composite.frag.spv"),
	};
}

void BloomEffect::RecreatePipelines() {
	// Pipelines are owned by the graph; RenderGraph::RecreatePipelines() rebuilds
	// them. Keeping the override empty rather than removing it because
	// PostProcessEffect::RecreatePipelines is still part of the interface during
	// the §1.5 migration.
}
