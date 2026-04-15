#include "post-process/LensFlareEffect.h"
#include "RenderGraph.h"
#include "DescriptorSetBuilder.h"
#include "PipelineDefaults.h"
#include "Utils.h"
#include "config.h"

namespace {

struct FlarePC {
	glm::vec2 sunScreen;
	float sunVisible;
	float intensity;
	float haloRadius;
	float ghostSpread;
	float aspect;
	float _pad;
};

std::string ShaderPath(const char* name) {
	return std::string(config::SHADER_DIR) + "/" + name;
}

}  // namespace

ImageHandle LensFlareEffect::RegisterPasses(
	RenderGraph& graph,
	const PostProcessContext& ctx,
	ImageHandle input,
	VkExtent2D extent)
{
	m_device = ctx.device;
	m_camera = ctx.camera;
	m_lighting = ctx.lighting;
	m_extent = extent;
	m_maxFramesInFlight = ctx.maxFramesInFlight;
	m_input = input;

	if (!m_sampler) {
		m_sampler = VWrap::Sampler::Create(ctx.device);
	}

	m_output = graph.CreateImage("lens_flare_out",
		{ extent.width, extent.height, 1, ctx.sceneFormat });

	auto& b = graph.AddGraphicsPass("Lens Flare")
		.SetColorAttachment(m_output, LoadOp::Clear, StoreOp::Store)
		.Read(m_input)
		.SetRecord([this](PassContext& pc) {
			auto cmd = pc.cmd->Get();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->Get());
			VkDescriptorSet ds = m_sets[pc.frameIndex]->Get();
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			FlarePC c{};
			c.intensity = m_intensity;
			c.haloRadius = m_haloRadius;
			c.ghostSpread = m_ghostSpread;
			c.aspect = float(pc.extent.width) / float(pc.extent.height);

			if (m_lighting && m_camera) {
				// Project a distant point along the sun direction through the current view-proj.
				// If it lands behind the camera or outside a generous screen margin, the
				// shader treats the flare as invisible.
				glm::vec3 sunDir = m_lighting->GetSunDirection();
				glm::vec3 worldPoint = m_camera->GetPosition() + sunDir * 1000.0f;
				glm::mat4 vp = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
				glm::vec4 clip = vp * glm::vec4(worldPoint, 1.0f);
				if (clip.w > 0.0f) {
					glm::vec3 ndc = glm::vec3(clip) / clip.w;
					// NDC Y is already flipped by the projection matrix (m[1][1] *= -1)
					// so ndc.xy maps to screen UV directly.
					c.sunScreen = glm::vec2(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
					c.sunVisible = (m_lighting->sunIntensity > 0.0f) ? 1.0f : 0.0f;
				} else {
					c.sunScreen = glm::vec2(0.5f);
					c.sunVisible = 0.0f;
				}
			} else {
				c.sunScreen = glm::vec2(0.5f);
				c.sunVisible = 0.0f;
			}

			vkCmdPushConstants(cmd, m_pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
			vkCmdDraw(cmd, 4, 1, 0, 0);
		});
	m_renderPass = b.GetRenderPassPtr();

	auto res = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(m_maxFramesInFlight);
	m_layout = res.layout;
	m_pool = res.pool;
	m_sets = res.sets;

	CreatePipeline();
	return m_output;
}

void LensFlareEffect::CreatePipeline() {
	auto vertCode = VWrap::readFile(ShaderPath("post_fullscreen.vert.spv"));
	auto fragCode = VWrap::readFile(ShaderPath("lens_flare.frag.spv"));

	VkPushConstantRange push{};
	push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = sizeof(FlarePC);

	auto info = PipelineDefaults::FullscreenQuad(m_renderPass, m_layout, m_extent, { push });
	m_pipeline = VWrap::Pipeline::Create(m_device, info, vertCode, fragCode);
}

void LensFlareEffect::WriteGraphDescriptors(RenderGraph& graph) {
	auto inputView = graph.GetImageView(m_input);
	for (auto& ds : m_sets) {
		VkDescriptorImageInfo info{};
		info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		info.imageView = inputView->Get();
		info.sampler = m_sampler->Get();

		VkWriteDescriptorSet w{};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = ds->Get();
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &info;
		vkUpdateDescriptorSets(m_device->Get(), 1, &w, 0, nullptr);
	}
}

std::vector<TechniqueParameter>& LensFlareEffect::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Flare Enabled",    TechniqueParameter::Bool,  &m_enabled },
			{ "Flare Intensity",  TechniqueParameter::Float, &m_intensity,   0.0f, 2.0f },
			{ "Halo Radius",      TechniqueParameter::Float, &m_haloRadius,  0.0f, 0.5f },
			{ "Ghost Spread",     TechniqueParameter::Float, &m_ghostSpread, 0.0f, 2.0f },
		};
	}
	return m_parameters;
}

std::vector<std::string> LensFlareEffect::GetShaderPaths() const {
	return {
		ShaderPath("post_fullscreen.vert.spv"),
		ShaderPath("lens_flare.frag.spv"),
	};
}

void LensFlareEffect::RecreatePipelines() {
	if (!m_device) return;
	m_pipeline.reset();
	CreatePipeline();
}
