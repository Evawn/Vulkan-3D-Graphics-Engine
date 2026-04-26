#pragma once

#include "RenderGraphTypes.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>

// ---- RenderItem ----
//
// A drawable atom. The split between scene (producer) and technique (consumer)
// crosses this struct: the future scene graph fills RenderItems and drops them
// into a RenderScene; render passes pull items out by type and submit draws.
//
// Items are POD by design: no virtuals, no inheritance, no owned resources.
// All resource references are graph handles (BufferHandle / ImageHandle), so
// items can be cheaply copied and stored in flat vectors. The expectation is
// that millions of grass-blade items fit naturally — per-instance state lives
// in pass-owned SSBOs (firstInstance + instanceCount addresses into them),
// not in the item itself.

enum class RenderItemType : uint8_t {
	Mesh,                // Indexed triangle mesh with CPU-uploaded geometry (e.g. OBJ models).
	Fullscreen,          // Full-screen quad — ray-march tracers, post-process effects.
	InstancedVoxelMesh,  // Bounding-volume rasterized voxel volume + per-instance SSBO; for foliage.
	BrickmapVolume,      // Reserved — bounded brickmap rendering (vs the current full-screen tracer).

	Count_  // sentinel for bucketing — must be last
};

inline constexpr size_t kRenderItemTypeCount = static_cast<size_t>(RenderItemType::Count_);

struct RenderItem {
	RenderItemType type = RenderItemType::Mesh;

	// --- Geometry (Mesh, InstancedVoxelMesh) ---
	BufferHandle  vertexBuffer;
	BufferHandle  indexBuffer;
	uint32_t      indexCount       = 0;
	uint32_t      firstIndex       = 0;
	int32_t       vertexOffset     = 0;

	// --- Instancing (InstancedVoxelMesh, future: instanced Mesh) ---
	// Per-instance data lives in a pass-owned SSBO; the item only addresses into
	// that SSBO via firstInstance/instanceCount. This is the line of code that
	// makes "millions of grass blades" fall out as a single vkCmdDrawIndexed.
	uint32_t      instanceCount    = 1;
	uint32_t      firstInstance    = 0;

	// --- Voxel asset reference (InstancedVoxelMesh, BrickmapVolume) ---
	// Animation frames live as Z-slices or array layers within voxelAsset; the
	// per-instance SSBO carries the per-instance frame index that the shader
	// uses to sample.
	ImageHandle   voxelAsset;
	uint32_t      frameCount       = 1;

	// --- Per-item transform (Mesh; ignored for Fullscreen) ---
	glm::mat4     transform        = glm::mat4(1.0f);

	// --- AABB (InstancedVoxelMesh — sizes the bounding-box rasterization draw) ---
	glm::vec3     aabbMin          = glm::vec3(0.0f);
	glm::vec3     aabbMax          = glm::vec3(0.0f);
};

// ---- Draw helpers ----
//
// Free functions, not members of any pass or technique. The future scene-graph
// machinery will call the same helpers; routing happens via RenderItemType.
// Helpers assume the pass record callback has already bound the pipeline,
// descriptor sets, and any per-pass push constants — they only emit the draw.

class RenderGraph;

void DrawMeshItem           (const PassContext& ctx, const RenderItem& item, const RenderGraph& graph);
void DrawFullscreenItem     (const PassContext& ctx, const RenderItem& item);
void DrawInstancedVoxelMesh (const PassContext& ctx, const RenderItem& item, const RenderGraph& graph);
