#pragma once

#include "PostProcessEffect.h"
#include "FullscreenPass.h"
#include "Sampler.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

// Bright-pass + separable Gaussian blur + additive composite. Four graph passes,
// all in LDR (same format as the input scene image) for v1. Intermediate images
// are full-res for simplicity; drop to half/quarter-res later if it becomes a perf
// issue.
class BloomEffect : public PostProcessEffect {
public:
	std::string GetName() const override { return "Bloom"; }

	ImageHandle RegisterPasses(
		RenderGraph& graph,
		const PostProcessContext& ctx,
		ImageHandle input,
		VkExtent2D extent) override;

	std::vector<TechniqueParameter>& GetParameters() override;
	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipelines() override;

private:
	// Image handles threaded through the chain. m_input is captured from the
	// argument so WriteGraphDescriptors can rebind it after graph.Compile().
	ImageHandle m_input;
	ImageHandle m_bright;
	ImageHandle m_blurH;
	ImageHandle m_bloom;
	ImageHandle m_output;

	std::shared_ptr<VWrap::Sampler> m_sampler;

	std::unique_ptr<FullscreenPass> m_brightPass;
	std::unique_ptr<FullscreenPass> m_blurHPass;
	std::unique_ptr<FullscreenPass> m_blurVPass;
	std::unique_ptr<FullscreenPass> m_compositePass;

	float m_threshold = 0.93f;
	float m_knee = 0.19f;
	float m_intensity = 1.6f;
	float m_blurRadius = 3.4f;

	std::vector<TechniqueParameter> m_parameters;
};
