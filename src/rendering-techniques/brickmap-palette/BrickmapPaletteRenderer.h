#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "BindingTable.h"
#include "Buffer.h"
#include "CommandBuffer.h"
#include "VoxLoader.h"
#include "PaletteResource.h"
#include "SceneLighting.h"
#include <chrono>
#include <memory>
#include <glm/glm.hpp>

class RenderGraph;

class BrickmapPaletteRenderer : public RenderTechnique {
private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<Camera> m_camera;

	// Graph-managed 128^3 3D storage image (R8_UINT: material index per voxel)
	ImageHandle m_volume;
	// Graph-managed brickmap structure buffer (top grid + packed material bytes)
	BufferHandle m_brickmap_buffer;
	VkBuffer m_brickmap_vk_buffer = VK_NULL_HANDLE;

	// Shared 256-entry palette texture + its sampler.
	std::unique_ptr<PaletteResource> m_palette;

	// Descriptor wiring is owned by BindingTables; pipelines by the graph.
	std::shared_ptr<BindingTable> m_compute_bindings;
	std::shared_ptr<BindingTable> m_build_bindings;
	std::shared_ptr<BindingTable> m_graphics_bindings;

	// Tunable parameters
	int m_shape = 0;
	int m_max_iterations = 250;
	float m_sky_color[3] = { 0.529f, 0.808f, 0.922f };
	bool m_debug_color = false;
	float m_time_scale = 1.0f;
	std::chrono::steady_clock::time_point m_start_time;

	// Dynamic per-axis volume sizing (procedural defaults to uvec3(128); .vox fits per-axis)
	glm::uvec3 m_volume_size = glm::uvec3(128, 128, 128);

	// .vox file import state. m_loaded_vox holds the active .vox model for as
	// long as it's loaded — OnPostCompile re-applies it on every graph rebuild
	// (e.g. after a window resize), so the volume always matches the file.
	std::string m_vox_file_path;
	bool m_pending_reload = false;
	std::optional<VoxModel> m_loaded_vox;
	RenderGraph* m_graph = nullptr;

	// Shared, non-owning pointer to renderer-owned lighting state. Captured from
	// RenderContext during RegisterPasses; read during per-frame record to
	// populate sun push constants. Survives RegisterPasses lifetime because the
	// Renderer owns it.
	const SceneLighting* m_lighting = nullptr;

	std::vector<TechniqueParameter> m_parameters;

	void UploadVolumeData(const uint8_t* data);

public:
	std::string GetDisplayName() const override { return "Brickmap Palette Renderer"; }

	void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		ImageHandle colorTarget,
		ImageHandle depthTarget,
		ImageHandle resolveTarget) override;

	void Reload(const RenderContext& ctx) override;

	// Post-Compile hook: applies any pending .vox upload now that graph resources
	// are allocated. Descriptor wiring is handled automatically by BindingTable.
	void OnPostCompile(RenderGraph& graph) override;

	std::vector<std::string> GetShaderPaths() const override;

	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
};
