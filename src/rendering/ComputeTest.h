#pragma once

#include "RenderTechnique.h"
#include "ComputePipeline.h"
#include "Pipeline.h"
#include "DescriptorSet.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "Sampler.h"
#include "Image.h"
#include "ImageView.h"

class ComputeTest : public RenderTechnique {
private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<VWrap::CommandPool> m_compute_pool;
	std::shared_ptr<VWrap::RenderPass> m_render_pass;
	VkExtent2D m_extent{};

	// Storage image (written by compute, read by fragment)
	std::shared_ptr<VWrap::Image> m_storage_image;
	std::shared_ptr<VWrap::ImageView> m_storage_image_view;
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

	void CreateStorageImage();
	void CreateComputePipeline();
	void CreateComputeDescriptors();
	void CreateGraphicsPipeline();
	void CreateGraphicsDescriptors(uint32_t max_sets);
	void WriteDescriptors();
	void DispatchCompute();

public:
	std::string GetName() const override { return "Compute Test"; }

	void Init(const RenderContext& ctx) override;
	void Shutdown() override {}
	void OnResize(VkExtent2D newExtent) override;

	void RecordCommands(
		std::shared_ptr<VWrap::CommandBuffer> cmd,
		uint32_t frameIndex,
		std::shared_ptr<Camera> camera) override;

	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipeline(const RenderContext& ctx) override;

	FrameStats GetFrameStats() const override;
};
