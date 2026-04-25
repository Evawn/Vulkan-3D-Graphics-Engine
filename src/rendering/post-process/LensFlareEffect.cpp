#include "post-process/LensFlareEffect.h"
#include "RenderGraph.h"
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
	m_camera = ctx.camera;
	m_lighting = ctx.lighting;
	m_input = input;

	if (!m_sampler) {
		m_sampler = VWrap::Sampler::CreateLinearClamp(ctx.device);
	}

	m_output = graph.CreateImage("lens_flare_out",
		{ extent.width, extent.height, 1, ctx.sceneFormat });

	m_pass = FullscreenPass::Build(
		graph, ctx.device, extent, ctx.maxFramesInFlight,
		FullscreenPassDesc{
			"Lens Flare", m_output, { m_input }, m_sampler,
			ShaderPath("lens_flare.frag.spv"), sizeof(FlarePC) },
		[this](PassContext& pc, VkPipelineLayout layout) {
			FlarePC c{};
			c.intensity = m_intensity;
			c.haloRadius = m_haloRadius;
			c.ghostSpread = m_ghostSpread;
			c.aspect = float(pc.extent.width) / float(pc.extent.height);

			if (m_lighting && m_camera) {
				// Project a distant point along the sun direction through the
				// current view-proj. If it lands behind the camera or outside
				// a generous screen margin, the shader treats the flare as
				// invisible.
				glm::vec3 sunDir = m_lighting->GetSunDirection();
				glm::vec3 worldPoint = m_camera->GetPosition() + sunDir * 1000.0f;
				glm::mat4 vp = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
				glm::vec4 clip = vp * glm::vec4(worldPoint, 1.0f);
				if (clip.w > 0.0f) {
					glm::vec3 ndc = glm::vec3(clip) / clip.w;
					// NDC Y is already flipped by the projection matrix
					// (m[1][1] *= -1) so ndc.xy maps to screen UV directly.
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

			pc.cmd->CmdPushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, &c, sizeof(c));
		});

	return m_output;
}

void LensFlareEffect::WriteGraphDescriptors(RenderGraph& graph) {
	m_pass->WriteDescriptors(graph);
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
	if (m_pass) m_pass->RecreatePipeline();
}
