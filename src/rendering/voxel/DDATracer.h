#pragma once
#include "RenderTechnique.h"
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

	void Init(const RenderContext& ctx) override;
	void Shutdown() override {}
	void OnResize(VkExtent2D newExtent) override { m_extent = newExtent; }

	void RecordCommands(
		std::shared_ptr<VWrap::CommandBuffer> cmd,
		uint32_t frameIndex,
		std::shared_ptr<Camera> camera) override;

	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipeline(const RenderContext& ctx) override;

	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
};
