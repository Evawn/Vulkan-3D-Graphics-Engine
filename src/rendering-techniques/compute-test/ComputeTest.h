#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "DescriptorSetBuilder.h"
#include "ComputePipeline.h"
#include "Pipeline.h"
#include "Sampler.h"
#include "Image.h"
#include "ImageView.h"

class ComputeTest : public RenderTechnique {
private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<VWrap::RenderPass> m_render_pass;
	VkExtent2D m_extent{};

	// Storage image handle (graph-managed)
	ImageHandle m_storage;

	std::shared_ptr<VWrap::Sampler> m_sampler;

	// Compute pipeline + descriptors
	std::shared_ptr<VWrap::ComputePipeline> m_compute_pipeline;
	std::shared_ptr<VWrap::DescriptorSetLayout> m_compute_descriptor_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_compute_descriptor_pool;
	std::shared_ptr<VWrap::DescriptorSet> m_compute_descriptor_set;

	// Graphics pipeline + descriptors (per-frame for double buffering)
	std::shared_ptr<VWrap::Pipeline> m_graphics_pipeline;
	std::shared_ptr<VWrap::DescriptorSetLayout> m_graphics_descriptor_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_graphics_descriptor_pool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_graphics_descriptor_sets;

	void CreateComputePipeline();
	void CreateGraphicsPipeline();

public:
	std::string GetName() const override { return "Compute Test"; }

	void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		ImageHandle colorTarget,
		ImageHandle depthTarget,
		ImageHandle resolveTarget) override;

	void Shutdown() override {}
	void OnResize(VkExtent2D newExtent, RenderGraph& graph) override;

	void WriteGraphDescriptors(RenderGraph& graph) override;

	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipeline(const RenderContext& ctx) override;

	FrameStats GetFrameStats() const override;
};
