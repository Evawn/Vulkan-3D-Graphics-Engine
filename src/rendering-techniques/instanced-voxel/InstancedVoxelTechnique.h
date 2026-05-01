#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "AssetRegistry.h"
#include "Scene.h"
#include "BindingTable.h"
#include "PaletteResource.h"
#include "SceneLighting.h"
#include "SkyDescription.h"
#include <chrono>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

class RenderGraph;

// InstancedVoxelTechnique
//
// Foundation technique for the future foliage workflow. Consumes
// RenderItem::InstancedVoxelMesh items emitted by the SceneExtractor from
// scene nodes carrying an InstanceCloudComponent. Each item triggers one
// `vkCmdDraw(36, instanceCount, 0, firstInstance)` — the vertex shader
// procedurally generates a unit cube per instance from gl_VertexIndex,
// scales by the per-instance AABB, transforms by per-instance TRS pulled
// from the item's instance SSBO. The fragment shader DDAs into the voxel
// asset's volume image, indexing into the right Z-slab per the
// per-instance frame offset.
//
// v1 scope:
//   - One InstanceCloud per scene (the technique seeds it during first
//     RegisterPasses with a 32×32 grid of "grass blades").
//   - One animated voxel asset (16×32×16 with 8 frames), generated once by
//     a compute pass at graph rebuild.
//   - No bindless: a single voxel asset binding per draw. Multi-species
//     foliage will need descriptor indexing + per-instance assetIndex.
//
// What this technique proves: the InstancedVoxelMesh consumer pipeline,
// per-instance SSBO addressing, frame-as-Z-slab animation, and that a new
// rendering technique can be added without touching anything else in
// src/rendering/.
class InstancedVoxelTechnique : public RenderTechnique {
private:
	std::shared_ptr<VWrap::Device>      m_device;
	std::shared_ptr<VWrap::Allocator>   m_allocator;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<Camera>             m_camera;

	AssetRegistry* m_assets = nullptr;
	Scene*         m_world  = nullptr;

	// Animated voxel asset — 16×32×16 voxels, 8 frames, R8_UINT, palette-indexed.
	// Procedurally written by a compute pass during graph rebuild.
	AssetID     m_volume_asset;
	ImageHandle m_volume;     // resolved each rebuild from the registry
	glm::uvec3  m_volume_size = glm::uvec3(16, 32, 16);
	uint32_t    m_frame_count = 8;

	// Per-instance SSBO. Owned by the registry as a Persistent buffer; we
	// re-resolve the BufferHandle on each graph rebuild.
	BufferHandle m_instance_buffer;
	uint32_t     m_instance_count = 0;

	// Per-asset, per-frame occupancy bitmask. One bit per voxel, packed 32 along
	// X (see instanced_voxel_generate.comp for layout). Sibling to the volume
	// image — the volume holds palette material for the renderer; the bitmask
	// holds occupancy for the substrate's shadow query (LIGHTING.md §3.2).
	// Private to the technique for v1; graduates to AssetRegistry when
	// multi-species lands.
	BufferHandle m_bitmask_buffer;
	uint32_t     m_bitmask_word_count = 0;

	// World-grid occupancy substrate (LIGHTING.md §3). Holds a top-level brick
	// grid spanning the cloud's footprint plus a CSR-style per-brick foliage
	// instance overlap list. Built host-side once per `RebuildInstanceData`,
	// uploaded as a single staging copy. Persistent so it survives viewport
	// resize.
	//
	// Sized at an upper bound at allocation time (see Substrate.h —
	// SubstrateUpperBoundWords). The actual populated extent is recorded in
	// the buffer header; the trailing words are unread.
	BufferHandle m_substrate_buffer;
	uint32_t     m_substrate_word_capacity = 0;

	// Scene node carrying the InstanceCloudComponent. Created once during
	// the first RegisterPasses; the extractor emits one InstancedVoxelMesh
	// item per frame from this node.
	SceneNode* m_node = nullptr;

	// Bindings.
	std::shared_ptr<BindingTable> m_compute_bindings;   // for the generate compute pass
	std::shared_ptr<BindingTable> m_sky_bindings;       // fullscreen sky pre-pass
	std::shared_ptr<BindingTable> m_graphics_bindings;  // for the draw pass

	// Volume meta uniform (per-frame UBO, stays static after first rebuild —
	// per-frame slot lets BindingTable's per-frame uniform path serve it).
	std::vector<std::shared_ptr<VWrap::Buffer>> m_meta_buffers;
	std::vector<void*>                          m_meta_mapped;

	// Per-frame UBO carrying camera/sun/sky/time/iteration state. Rewritten
	// every frame in the trace pass record callback. Bound to both the trace
	// pass (for shading) and the sky pre-pass (for the sun disk + gradient).
	std::vector<std::shared_ptr<VWrap::Buffer>> m_frame_ubo_buffers;
	std::vector<void*>                          m_frame_ubo_mapped;

	// Palette.
	std::unique_ptr<PaletteResource> m_palette;

	// NEAREST sampler for the integer volume; the palette uses its own internal sampler.
	std::shared_ptr<VWrap::Sampler> m_volume_sampler;

	const SceneLighting*  m_lighting = nullptr;
	const SkyDescription* m_sky      = nullptr;

	// Tunables.
	int   m_max_iterations = 96;
	bool  m_debug_color = false;
	float m_animation_speed = 6.0f;   // frames per second
	int   m_grid_dim = 128;           // 128×128 = 16384 instances
	// Spacing between adjacent blade origins, in *world voxels*. The asset is
	// 16 voxels wide but the painted blade is ~1–2 voxels wide
	// (instanced_voxel_generate.comp's tapered cross-section), so a pitch
	// half the asset width packs blades 4× tighter without colliding paint —
	// only the AABB bounding boxes overlap. Combined with grid_dim=128, this
	// gives the same cloud footprint as the historical 64/16 config but at
	// 4× density.
	int   m_blade_pitch_voxels = 8;
	bool  m_pending_grid_rebuild = false;

	// Shadow tunables. The toggle is technique-local so the user finds it
	// next to the other technique parameters (the global SceneLighting
	// "Sun Shadows" still drives brickmap shadows; the trace shader uses
	// the AND of both). Bias values are receiver-side in world units.
	//
	// Post-Milestone D: shadows resolve via the substrate's `traceShadowWorld`
	// (substrate.glsl) rather than a depth-map sample. The bias parameters
	// retain their meaning — receiver-side normal-direction offset to keep
	// the shadow ray from re-hitting the receiver's own voxel.
	bool  m_shadows_enabled       = true;
	// Bias is now sub-voxel — the substrate's shadow ray starts from the actual
	// fractional entry-face hit point (instanced_voxel.frag computes it from
	// `h.entryT`), so we only need enough offset to escape numerical precision
	// at the face boundary. Compare to the brickmap pillar, which uses a fixed
	// 0.01 × voxelWorldSize ≈ 0.000125 with no slope term and no acne. Slope
	// term retained as a small safety margin at grazing angles where the ray
	// can clip neighbouring voxels of the same blade.
	float m_shadow_bias_constant  = 0.001f;
	float m_shadow_bias_slope     = 0.005f;

	std::chrono::steady_clock::time_point m_start_time;

	std::vector<TechniqueParameter> m_parameters;

	// Builds the host-side per-instance data and uploads it to the graph buffer.
	void RebuildInstanceData(RenderGraph& graph);

	// Computes camera/sun/sky/time state and writes it into the per-frame UBO
	// at the given index. Called from the FIRST pass to record each frame
	// (the sky pre-pass) and idempotently again by the trace pass — keeping
	// it idempotent decouples correctness from the graph's pass ordering.
	void WritePerFrameUbo(uint32_t frameIndex);

public:
	std::string GetDisplayName() const override { return "Instanced Voxel"; }

	RenderTargetDesc DescribeTargets(const RendererCaps& caps) const override;
	void RegisterPasses(RenderGraph& graph, const RenderContext& ctx,
	                    const TechniqueTargets& targets) override;
	void OnPostCompile(RenderGraph& graph) override;
	void Reload(const RenderContext& ctx) override;

	std::vector<std::string> GetShaderPaths() const override;
	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
};
