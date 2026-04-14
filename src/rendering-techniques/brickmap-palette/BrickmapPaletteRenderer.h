#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "DescriptorSetBuilder.h"
#include "ComputePipeline.h"
#include "Pipeline.h"
#include "Sampler.h"
#include "Image.h"
#include "ImageView.h"
#include "Buffer.h"
#include "CommandBuffer.h"
#include "VoxLoader.h"
#include <chrono>

class RenderGraph;

class BrickmapPaletteRenderer : public RenderTechnique {
private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<VWrap::RenderPass> m_render_pass;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	VkExtent2D m_extent{};
	std::shared_ptr<Camera> m_camera;

	// Graph-managed 128^3 3D storage image (R8_UINT: material index per voxel)
	ImageHandle m_volume;
	// Graph-managed brickmap structure buffer (top grid + packed material bytes)
	BufferHandle m_brickmap_buffer;
	VkBuffer m_brickmap_vk_buffer = VK_NULL_HANDLE;

	std::shared_ptr<VWrap::Sampler> m_sampler;

	// Palette texture (256x1 RGBA8, uploaded once)
	std::shared_ptr<VWrap::Image> m_palette_image;
	std::shared_ptr<VWrap::ImageView> m_palette_image_view;

	// Generate pipeline + descriptors (compute: writes volume)
	std::shared_ptr<VWrap::ComputePipeline> m_compute_pipeline;
	std::shared_ptr<VWrap::DescriptorSetLayout> m_compute_descriptor_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_compute_descriptor_pool;
	std::shared_ptr<VWrap::DescriptorSet> m_compute_descriptor_set;

	// Build pipeline + descriptors (compute: reads volume, writes brickmap buffer)
	std::shared_ptr<VWrap::ComputePipeline> m_build_pipeline;
	std::shared_ptr<VWrap::DescriptorSetLayout> m_build_descriptor_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_build_descriptor_pool;
	std::shared_ptr<VWrap::DescriptorSet> m_build_descriptor_set;

	// Graphics pipeline + descriptors (per-frame)
	std::shared_ptr<VWrap::Pipeline> m_graphics_pipeline;
	std::shared_ptr<VWrap::DescriptorSetLayout> m_graphics_descriptor_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_graphics_descriptor_pool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_graphics_descriptor_sets;

	// Tunable parameters
	int m_shape = 0;
	int m_max_iterations = 250;
	float m_sky_color[3] = { 0.529f, 0.808f, 0.922f };
	bool m_debug_color = true;
	float m_time_scale = 1.0f;
	std::chrono::steady_clock::time_point m_start_time;

	// Dynamic volume sizing
	uint32_t m_volume_size = 128;

	// .vox file import state
	std::string m_vox_file_path;
	bool m_needs_reload = false;
	bool m_needs_rebuild = false;
	bool m_vox_active = false;
	std::optional<VoxModel> m_pending_vox;
	RenderGraph* m_graph = nullptr;

	std::vector<TechniqueParameter> m_parameters;

	void CreateComputePipeline();
	void CreateBuildPipeline();
	void CreateGraphicsPipeline();
	void CreatePaletteTexture();
	void UploadVolumeData(const uint8_t* data);
	void UploadPalette(const uint8_t* rgbaData);

public:
	std::string GetName() const override { return "Brickmap Palette Renderer"; }

	void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		ImageHandle colorTarget,
		ImageHandle depthTarget,
		ImageHandle resolveTarget) override;

	void Shutdown() override {}
	void OnResize(VkExtent2D newExtent, RenderGraph& graph) override;

	bool NeedsReload() const override { return m_needs_reload; }
	void PerformReload(const RenderContext& ctx) override;
	bool NeedsRebuild() const override { return m_needs_rebuild; }

	void WriteGraphDescriptors(RenderGraph& graph) override;

	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipeline(const RenderContext& ctx) override;

	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
};
