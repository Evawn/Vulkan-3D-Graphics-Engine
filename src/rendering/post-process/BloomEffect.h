#pragma once

#include "PostProcessEffect.h"
#include "Pipeline.h"
#include "Sampler.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "DescriptorSet.h"
#include "RenderPass.h"
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

	void WriteGraphDescriptors(RenderGraph& graph) override;
	std::vector<TechniqueParameter>& GetParameters() override;
	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipelines() override;

private:
	// Aggregate of per-pass Vulkan objects. Each pass is a fullscreen quad
	// reading 1-2 sampled images and writing one color attachment.
	struct Pass {
		std::shared_ptr<VWrap::RenderPass> renderPass;
		std::shared_ptr<VWrap::Pipeline> pipeline;
		std::shared_ptr<VWrap::DescriptorSetLayout> layout;
		std::shared_ptr<VWrap::DescriptorPool> pool;
		std::vector<std::shared_ptr<VWrap::DescriptorSet>> sets;  // one per frame-in-flight
	};

	void CreatePassPipeline(Pass& p, const std::string& fragSpv, size_t pushSize);

	// Context captured at RegisterPasses — needed for pipeline recreation on
	// hot-reload, where we only have the effect instance.
	std::shared_ptr<VWrap::Device> m_device;
	VkExtent2D m_extent{};
	uint32_t m_maxFramesInFlight = 0;

	// Image handles threaded through the chain. m_input is captured from the
	// argument so WriteGraphDescriptors can rebind it after graph.Compile().
	ImageHandle m_input;
	ImageHandle m_bright;
	ImageHandle m_blurH;
	ImageHandle m_bloom;
	ImageHandle m_output;

	std::shared_ptr<VWrap::Sampler> m_sampler;

	Pass m_brightPass;
	Pass m_blurHPass;
	Pass m_blurVPass;
	Pass m_compositePass;   // uses a 2-binding descriptor layout

	float m_threshold = 0.93f;
	float m_knee = 0.19f;
	float m_intensity = 1.6f;
	float m_blurRadius = 3.4f;

	std::vector<TechniqueParameter> m_parameters;
};
