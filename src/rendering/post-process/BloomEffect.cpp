#include "post-process/BloomEffect.h"
#include "RenderGraph.h"
#include "DescriptorSetBuilder.h"
#include "PipelineDefaults.h"
#include "Utils.h"
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
	m_device = ctx.device;
	m_extent = extent;
	m_maxFramesInFlight = ctx.maxFramesInFlight;
	m_input = input;

	if (!m_sampler) {
		m_sampler = VWrap::Sampler::Create(ctx.device);
	}

	// All intermediates are full-res and in the same format as the scene. The
	// graph auto-derives COLOR_ATTACHMENT + SAMPLED usage from the pass
	// declarations below.
	ImageDesc desc{ extent.width, extent.height, 1, ctx.sceneFormat };
	m_bright = graph.CreateImage("bloom_bright",    desc);
	m_blurH  = graph.CreateImage("bloom_blur_h",    desc);
	m_bloom  = graph.CreateImage("bloom_blur_v",    desc);
	m_output = graph.CreateImage("bloom_composite", desc);

	// Helper: register a fullscreen-quad pass writing `color`, reading `reads`,
	// and capture the render pass pointer into `pass.renderPass`.
	auto addPass = [&](const std::string& name,
	                   ImageHandle color,
	                   std::initializer_list<ImageHandle> reads,
	                   Pass& pass,
	                   std::function<void(PassContext&)> record)
	{
		auto& b = graph.AddGraphicsPass(name)
			.SetColorAttachment(color, LoadOp::Clear, StoreOp::Store);
		for (auto h : reads) b.Read(h);
		b.SetRecord(std::move(record));
		pass.renderPass = b.GetRenderPassPtr();
	};

	addPass("Bloom Bright", m_bright, { m_input }, m_brightPass,
		[this](PassContext& pc) {
			auto cmd = pc.cmd->Get();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_brightPass.pipeline->Get());
			VkDescriptorSet ds = m_brightPass.sets[pc.frameIndex]->Get();
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_brightPass.pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
			BrightPC c{ m_threshold, m_knee };
			vkCmdPushConstants(cmd, m_brightPass.pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
			vkCmdDraw(cmd, 4, 1, 0, 0);
		});

	addPass("Bloom Blur H", m_blurH, { m_bright }, m_blurHPass,
		[this](PassContext& pc) {
			auto cmd = pc.cmd->Get();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurHPass.pipeline->Get());
			VkDescriptorSet ds = m_blurHPass.sets[pc.frameIndex]->Get();
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_blurHPass.pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
			BlurPC c{};
			c.direction = glm::vec2(1.0f, 0.0f);
			c.texelSize = glm::vec2(1.0f / float(m_extent.width), 1.0f / float(m_extent.height));
			c.radius = m_blurRadius;
			vkCmdPushConstants(cmd, m_blurHPass.pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
			vkCmdDraw(cmd, 4, 1, 0, 0);
		});

	addPass("Bloom Blur V", m_bloom, { m_blurH }, m_blurVPass,
		[this](PassContext& pc) {
			auto cmd = pc.cmd->Get();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurVPass.pipeline->Get());
			VkDescriptorSet ds = m_blurVPass.sets[pc.frameIndex]->Get();
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_blurVPass.pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
			BlurPC c{};
			c.direction = glm::vec2(0.0f, 1.0f);
			c.texelSize = glm::vec2(1.0f / float(m_extent.width), 1.0f / float(m_extent.height));
			c.radius = m_blurRadius;
			vkCmdPushConstants(cmd, m_blurVPass.pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
			vkCmdDraw(cmd, 4, 1, 0, 0);
		});

	addPass("Bloom Composite", m_output, { m_input, m_bloom }, m_compositePass,
		[this](PassContext& pc) {
			auto cmd = pc.cmd->Get();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePass.pipeline->Get());
			VkDescriptorSet ds = m_compositePass.sets[pc.frameIndex]->Get();
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_compositePass.pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
			CompositePC c{ m_intensity };
			vkCmdPushConstants(cmd, m_compositePass.pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
			vkCmdDraw(cmd, 4, 1, 0, 0);
		});

	// ---- Descriptor layouts + pools + sets ----
	auto buildSingle = [&](Pass& p) {
		auto res = DescriptorSetBuilder(m_device)
			.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
			.Build(m_maxFramesInFlight);
		p.layout = res.layout;
		p.pool = res.pool;
		p.sets = res.sets;
	};
	buildSingle(m_brightPass);
	buildSingle(m_blurHPass);
	buildSingle(m_blurVPass);

	{
		auto res = DescriptorSetBuilder(m_device)
			.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
			.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
			.Build(m_maxFramesInFlight);
		m_compositePass.layout = res.layout;
		m_compositePass.pool = res.pool;
		m_compositePass.sets = res.sets;
	}

	// ---- Pipelines (render passes already created above via GetRenderPassPtr) ----
	CreatePassPipeline(m_brightPass,    "bloom_bright.frag.spv",    sizeof(BrightPC));
	CreatePassPipeline(m_blurHPass,     "bloom_blur.frag.spv",      sizeof(BlurPC));
	CreatePassPipeline(m_blurVPass,     "bloom_blur.frag.spv",      sizeof(BlurPC));
	CreatePassPipeline(m_compositePass, "bloom_composite.frag.spv", sizeof(CompositePC));

	return m_output;
}

void BloomEffect::CreatePassPipeline(Pass& p, const std::string& fragSpv, size_t pushSize) {
	auto vertCode = VWrap::readFile(ShaderPath("post_fullscreen.vert.spv"));
	auto fragCode = VWrap::readFile(ShaderPath(fragSpv.c_str()));

	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = static_cast<uint32_t>(pushSize);

	auto info = PipelineDefaults::FullscreenQuad(p.renderPass, p.layout, m_extent, { push });
	p.pipeline = VWrap::Pipeline::Create(m_device, info, vertCode, fragCode);
}

static void WriteSingleSampler(
	VkDevice device,
	const std::vector<std::shared_ptr<VWrap::DescriptorSet>>& sets,
	std::shared_ptr<VWrap::ImageView> view,
	std::shared_ptr<VWrap::Sampler> sampler)
{
	for (auto& ds : sets) {
		VkDescriptorImageInfo info{};
		info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		info.imageView = view->Get();
		info.sampler = sampler->Get();

		VkWriteDescriptorSet w{};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = ds->Get();
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &info;
		vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
	}
}

void BloomEffect::WriteGraphDescriptors(RenderGraph& graph) {
	auto inputView  = graph.GetImageView(m_input);
	auto brightView = graph.GetImageView(m_bright);
	auto blurHView  = graph.GetImageView(m_blurH);
	auto bloomView  = graph.GetImageView(m_bloom);

	WriteSingleSampler(m_device->Get(), m_brightPass.sets, inputView,  m_sampler);
	WriteSingleSampler(m_device->Get(), m_blurHPass.sets,  brightView, m_sampler);
	WriteSingleSampler(m_device->Get(), m_blurVPass.sets,  blurHView,  m_sampler);

	for (auto& ds : m_compositePass.sets) {
		VkDescriptorImageInfo scene{};
		scene.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		scene.imageView = inputView->Get();
		scene.sampler = m_sampler->Get();

		VkDescriptorImageInfo bloom{};
		bloom.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		bloom.imageView = bloomView->Get();
		bloom.sampler = m_sampler->Get();

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = ds->Get();
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[0].pImageInfo = &scene;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = ds->Get();
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[1].pImageInfo = &bloom;

		vkUpdateDescriptorSets(m_device->Get(), 2, writes, 0, nullptr);
	}
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
	if (!m_device) return;
	m_brightPass.pipeline.reset();
	m_blurHPass.pipeline.reset();
	m_blurVPass.pipeline.reset();
	m_compositePass.pipeline.reset();
	CreatePassPipeline(m_brightPass,    "bloom_bright.frag.spv",    sizeof(BrightPC));
	CreatePassPipeline(m_blurHPass,     "bloom_blur.frag.spv",      sizeof(BlurPC));
	CreatePassPipeline(m_blurVPass,     "bloom_blur.frag.spv",      sizeof(BlurPC));
	CreatePassPipeline(m_compositePass, "bloom_composite.frag.spv", sizeof(CompositePC));
}
