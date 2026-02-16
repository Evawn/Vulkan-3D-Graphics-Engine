#pragma once
#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "DescriptorSet.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "Pipeline.h"
#include "Sampler.h"
#include "Image.h"
#include "ImageView.h"

class DDATracer : public RenderTechnique
{
private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<VWrap::Allocator> m_allocator;

	std::shared_ptr<VWrap::DescriptorSetLayout> m_descriptor_set_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_descriptor_pool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_descriptor_sets;

	std::shared_ptr<VWrap::Pipeline> m_pipeline;
	VkExtent2D m_extent;

	std::shared_ptr<VWrap::Image> m_brick_texture;
	std::shared_ptr<VWrap::ImageView> m_brick_texture_view;
	std::shared_ptr<VWrap::Sampler> m_sampler;

	std::shared_ptr<VWrap::RenderPass> m_render_pass;
	std::shared_ptr<Camera> m_camera;

	// Tunable parameters
	int m_max_iterations = 250;
	float m_sky_color[3] = { 0.529f, 0.808f, 0.922f };
	bool m_debug_color = true;

	std::vector<TechniqueParameter> m_parameters;

	void CreateDescriptors(int max_sets);
	void CreatePipeline(std::shared_ptr<VWrap::RenderPass> render_pass);
	void WriteDescriptors();

public:
	std::string GetName() const override { return "DDA Tracer"; }

	void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		ImageHandle colorTarget,
		ImageHandle depthTarget,
		ImageHandle resolveTarget) override;

	void Shutdown() override {}
	void OnResize(VkExtent2D newExtent, RenderGraph& graph) override { m_extent = newExtent; }

	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipeline(const RenderContext& ctx) override;

	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
};
