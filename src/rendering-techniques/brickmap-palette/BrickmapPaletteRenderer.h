#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "AssetRegistry.h"
#include "Scene.h"
#include "BindingTable.h"
#include "Buffer.h"
#include "CommandBuffer.h"
#include "VoxLoader.h"
#include "PaletteResource.h"
#include "SceneLighting.h"
#include "SkyDescription.h"
#include "PrimitiveFactory.h"
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

	// Volume lives in the AssetRegistry — file-loaded .vox swaps in via
	// AssetRegistry::ReplaceVoxelVolume; procedural mode swaps back via a
	// fresh CreateProceduralVoxelVolume. We hold the AssetID, look up the
	// live ImageHandle on each graph rebuild.
	AssetID     m_volume_asset;
	ImageHandle m_volume;            // resolved from m_volume_asset on each RegisterPasses
	// Pass-local scratch buffer: the build compute pass packs the volume into
	// a sparse brickmap structure each frame. Stays technique-owned because
	// it's derived data, not an asset.
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
	bool m_debug_color = false;
	float m_time_scale = 1.0f;
	std::chrono::steady_clock::time_point m_start_time;

	// Volume source — picks which content-producer fills the voxel asset.
	// The Source dropdown drives a rebuild of m_parameters so only the relevant
	// sub-controls are visible. Switching back to ProceduralSDF reuses the
	// existing "restore procedural" path (m_pending_reload + empty vox path).
	enum class Source : int { ProceduralSDF = 0, VoxFile = 1, IslandTerrain = 2 };
	int m_source = static_cast<int>(Source::ProceduralSDF);

	// Dynamic per-axis volume sizing (procedural defaults to uvec3(128); .vox fits per-axis)
	glm::uvec3 m_volume_size = glm::uvec3(128, 128, 128);

	// File-import state. The actual .vox payload (volume bytes + palette) lives
	// inside the AssetRegistry; we only hold the path + a pending-reload flag.
	// "Currently file-loaded?" is queried via the registry: if the asset's
	// VoxelVolumeAsset.isProcedural is false → file mode; true → procedural mode.
	std::string m_vox_file_path;
	bool m_pending_reload = false;

	// Island-terrain bake state. Config edited live by inspector sliders; bake
	// runs (CPU, on the calling thread) when m_pending_bake is set during Reload.
	IslandTerrainConfig m_terrain_cfg{};
	int  m_terrain_grid_x = 1024;     // mirror of cfg.gridSize.x as int for slider widget
	int  m_terrain_grid_y = 1024;
	int  m_terrain_max_height = 128;
	int  m_terrain_octaves = 5;
	int  m_terrain_seed    = 1337;
	bool m_pending_bake = false;
	RenderGraph*   m_graph    = nullptr;
	AssetRegistry* m_assets   = nullptr;
	Scene*         m_world    = nullptr;
	// Scene node carrying the VoxelVolume component. Created once during the
	// first RegisterPasses; the extractor emits one BrickmapVolume item per
	// frame from this.
	SceneNode*     m_node     = nullptr;

	// Shared, non-owning pointers to scene-owned state. Captured from
	// RenderContext during RegisterPasses; read during per-frame record to
	// populate sun + sky push constants. Survive RegisterPasses lifetime because
	// the Scene owns them.
	const SceneLighting*  m_lighting = nullptr;
	const SkyDescription* m_sky      = nullptr;

	std::vector<TechniqueParameter> m_parameters;
	// Cleared next time GetParameters() is called — rebuilds the param list.
	// Set from inside onChanged callbacks (e.g. Source dropdown) so we don't
	// invalidate the inspector's iterator mid-walk.
	bool m_params_dirty = true;

	void RebuildParameters();
	void BakeIslandTerrainNow();

public:
	std::string GetDisplayName() const override { return "Brickmap Palette Renderer"; }

	RenderTargetDesc DescribeTargets(const RendererCaps& caps) const override;

	void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		const TechniqueTargets& targets) override;

	void Reload(const RenderContext& ctx) override;

	// Post-Compile hook: enables/disables the procedural Generate pass based on
	// whether the underlying VoxelVolumeAsset is procedural or file-loaded.
	// Asset uploads themselves are now done by AssetRegistry::UploadPending.
	void OnPostCompile(RenderGraph& graph) override;

	std::vector<std::string> GetShaderPaths() const override;

	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
};
