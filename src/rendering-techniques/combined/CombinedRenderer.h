#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "AssetRegistry.h"
#include "Scene.h"
#include "BindingTable.h"
#include "Brickmap.h"
#include "ShadowBrickmap.h"
#include "PaletteResource.h"
#include "PrimitiveFactory.h"
#include "SceneLighting.h"
#include "SkyDescription.h"
#include <chrono>
#include <memory>
#include <glm/glm.hpp>

class RenderGraph;

// CombinedRenderer
//
// Milestone E from docs/LIGHTING.md, framed as a single technique that owns
// both pillars (CPU-baked island terrain + procedural animated foliage cloud)
// and renders them through one shared world-grid occupancy substrate. Both
// terrain hits and foliage hits compute their sun-shadow ray with the SAME
// `traceShadowWorld` (substrate.glsl with SUBSTRATE_TERRAIN), so terrain
// shadows fall on grass and grass shadows fall on terrain — voxel-precise.
//
// Coordinate convention (load-bearing — see LIGHTING.md §2):
//   - One world voxel pitch (kWorldVoxelSize, 1 inch).
//   - Terrain centered at world origin: `terrainOriginVoxel = -volumeSize/2`.
//   - Foliage cloud anchored at world origin (cloud-node translation = 0,
//     identity rotation, scale = kWorldVoxelSize). With no cloud-local offset,
//     the substrate's cloud-local voxel coords ARE world voxel coords — which
//     is what lets the unified shadow query test terrain bricks via the same
//     `voxel` variable it walks the substrate with.
//
// Pass list (in execution order):
//   1. Compute   — Foliage Generate (writes foliage volume + per-frame bitmask)
//   2. Graphics  — Sky pre-pass (clears color to sky gradient + sun disk)
//   3. Graphics  — Terrain Trace (fullscreen, brickmap DDA, writes color+depth)
//   4. Graphics  — Foliage Trace (instanced cubes, depth-tested against terrain)
//
// Coexists with the standalone BrickmapPaletteRenderer and InstancedVoxelTechnique
// (Application.cpp registers all three; user picks via inspector).
class CombinedRenderer : public RenderTechnique {
private:
	std::shared_ptr<VWrap::Device>      m_device;
	std::shared_ptr<VWrap::Allocator>   m_allocator;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<Camera>             m_camera;

	AssetRegistry*        m_assets   = nullptr;
	Scene*                m_world    = nullptr;
	const SceneLighting*  m_lighting = nullptr;
	const SkyDescription* m_sky      = nullptr;

	// ---- Terrain (CPU-baked island) ----
	BrickmapData m_terrain_brickmap;
	BufferHandle m_terrain_buffer;
	bool         m_terrain_pending_upload = false;
	bool         m_terrain_baked_ever     = false;

	// ---- Foliage asset ----
	// Same shape as InstancedVoxelTechnique's foliage asset: 16×32×16 voxels,
	// 8 frames. Reused procedural generate compute shader.
	AssetID      m_foliage_asset;
	ImageHandle  m_foliage_volume;
	glm::uvec3   m_foliage_size = glm::uvec3(16, 32, 16);
	uint32_t     m_foliage_frame_count = 8;

	// Per-asset, per-frame occupancy bitmask consumed by the substrate's
	// shadow query. Sibling to the volume image.
	BufferHandle m_foliage_bitmask_buffer;
	uint32_t     m_foliage_bitmask_word_count = 0;

	// ---- Foliage instances (placed surface-aware on the terrain) ----
	BufferHandle m_foliage_instance_buffer;
	uint32_t     m_foliage_instance_count    = 0;
	uint32_t     m_foliage_instance_capacity = 0;

	// Staged-but-not-uploaded instance + substrate bytes. RebuildFoliage-
	// Placement fills these; OnPostCompile uploads them once the graph has
	// allocated the destination buffers. Bytes (not GpuInstance vector) so the
	// header doesn't have to expose the std430 instance layout.
	std::vector<uint8_t>  m_pending_instance_bytes;
	std::vector<uint32_t> m_pending_substrate_data;
	bool                  m_foliage_pending_upload = false;

	// Per-axis instance count. Auto-fits to the island footprint when the
	// terrain bake completes — `m_foliage_grid_dim` is a tunable cap.
	int  m_foliage_grid_dim = 7;     // 7×7 = 49 instances ("dozens" per FEATURE.md scope)
	int  m_foliage_pitch_voxels = 0; // computed from terrain size / grid dim

	// ---- Shadow occupancy brickmap ----
	// One acceleration structure for ALL shadow queries (terrain + foliage).
	// Static pool baked from terrain at terrain bake time; dynamic pool
	// rewritten each frame by shadow_foliage_write.comp from the foliage
	// instances' current animation frame. See docs/SHADOW-BRICKS.md.
	BufferHandle m_shadow_buffer;
	uint32_t     m_shadow_word_capacity      = 0;
	BufferHandle m_instance_bricks_buffer;
	uint32_t     m_instance_brick_capacity   = 0;
	uint64_t     m_shadow_dynamic_pool_offset_bytes = 0;
	uint64_t     m_shadow_dynamic_pool_size_bytes   = 0;
	std::vector<uint32_t>                    m_pending_shadow_data;
	std::vector<ShadowBrickmap::InstanceBrick> m_pending_instance_bricks;
	bool         m_shadow_pending_upload     = false;
	uint32_t     m_pending_instance_brick_count = 0;

	// ---- Per-frame UBOs (camera/sun/sky/time + foliage meta) ----
	std::vector<std::shared_ptr<VWrap::Buffer>> m_frame_ubo_buffers;
	std::vector<void*>                          m_frame_ubo_mapped;
	std::vector<std::shared_ptr<VWrap::Buffer>> m_meta_ubo_buffers;
	std::vector<void*>                          m_meta_ubo_mapped;

	// ---- Palette + samplers ----
	std::unique_ptr<PaletteResource> m_palette;
	std::shared_ptr<VWrap::Sampler>  m_volume_sampler;

	// ---- Bindings ----
	std::shared_ptr<BindingTable> m_compute_bindings;          // foliage volume + asset bitmask gen
	std::shared_ptr<BindingTable> m_shadow_write_bindings;     // per-frame shadow dynamic-pool fill
	std::shared_ptr<BindingTable> m_sky_bindings;
	std::shared_ptr<BindingTable> m_terrain_bindings;
	std::shared_ptr<BindingTable> m_foliage_bindings;

	// Non-owning reference to the render graph; needed inside record
	// callbacks that have to resolve graph-managed buffers to VkBuffer
	// (e.g. shadow_foliage_write's vkCmdFillBuffer needs the actual handle).
	// Set by RegisterPasses; the graph outlives this technique.
	RenderGraph* m_graph = nullptr;

	// ---- Tunables ----
	IslandTerrainConfig m_terrain_cfg{};
	int  m_terrain_size       = 1024;   // voxels per X & Y (1024" ≈ 26 m at 1"/voxel)
	int  m_terrain_max_height = 128;    // voxels
	int  m_terrain_octaves    = 5;
	int  m_terrain_seed       = 1337;
	bool m_pending_bake       = true;   // bake on first frame

	int  m_max_iterations  = 256;          // primary trace outer cap
	int  m_max_shadow_brick_steps = 256;   // shadow trace outer cap (brick steps)
	bool m_debug_color     = false;
	float m_animation_speed = 0.5f;
	bool m_shadows_enabled = true;
	bool m_pending_grid_rebuild = false;

	std::chrono::steady_clock::time_point m_start_time;
	std::vector<TechniqueParameter>       m_parameters;

	// ---- Helpers ----

	// Bake the island terrain (PrimitiveFactory::BakeIslandTerrainBrickmap)
	// and stash the result in m_terrain_brickmap. Sets pending-upload + camera
	// reposition. Must precede RebuildFoliagePlacement so the placement walk
	// has the new terrain to sample.
	void BakeIslandNow();

	// Walk every (gx, gy) in the auto-fit foliage grid, find the topmost
	// solid voxel of that terrain column, and emit one instance there.
	// Triggers a shadow brickmap topology rebuild via RebuildShadowBrickmap.
	void RebuildFoliagePlacement();

	// Rebuild the shadow brickmap from the current terrain + foliage instance
	// set. Called when either input changes — terrain bake, instance edit.
	// Stashes the pending buffer bytes; OnPostCompile uploads them.
	void RebuildShadowBrickmap();

	void WriteFrameUbo(uint32_t frameIndex);

public:
	std::string      GetDisplayName() const override { return "Combined Renderer"; }
	RenderTargetDesc DescribeTargets(const RendererCaps& caps) const override;

	void RegisterPasses(RenderGraph& graph, const RenderContext& ctx,
	                    const TechniqueTargets& targets) override;
	void OnPostCompile(RenderGraph& graph) override;
	void Reload(const RenderContext& ctx) override;

	std::vector<std::string>         GetShaderPaths() const override;
	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats                        GetFrameStats() const override;
};
