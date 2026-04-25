#pragma once

#include "PostProcessEffect.h"
#include "FullscreenPass.h"
#include "Sampler.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

// Analytic lens flare: one fullscreen pass that reads the bloomed scene and adds
// a warm halo at the sun's projected position plus five chromatic ghosts marching
// along the sun→screen-center axis. Works in 2D screen space — no ray-marching
// into the scene is required.
class LensFlareEffect : public PostProcessEffect {
public:
	std::string GetName() const override { return "Lens Flare"; }

	ImageHandle RegisterPasses(
		RenderGraph& graph,
		const PostProcessContext& ctx,
		ImageHandle input,
		VkExtent2D extent) override;

	std::vector<TechniqueParameter>& GetParameters() override;
	std::vector<std::string> GetShaderPaths() const override;

private:
	std::shared_ptr<Camera> m_camera;
	SceneLighting* m_lighting = nullptr;

	ImageHandle m_input;
	ImageHandle m_output;

	std::shared_ptr<VWrap::Sampler> m_sampler;
	std::unique_ptr<FullscreenPass> m_pass;

	float m_intensity = 0.1f;
	float m_haloRadius = 0.15f;
	float m_ghostSpread = 0.85f;

	std::vector<TechniqueParameter> m_parameters;
};
