#pragma once

#include "RenderGraphTypes.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>

class BindingTable;

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
	SkinnedMesh,         // GPU-skinned indexed triangle mesh; joints come from the RenderScene arena.

	Count_  // sentinel for bucketing — must be last
};

inline constexpr size_t kRenderItemTypeCount = static_cast<size_t>(RenderItemType::Count_);

struct RenderItem {
	RenderItemType type = RenderItemType::Mesh;

	// --- Geometry (Mesh, InstancedVoxelMesh, SkinnedMesh) ---
	BufferHandle  vertexBuffer;
	BufferHandle  indexBuffer;
	uint32_t      indexCount       = 0;
	uint32_t      firstIndex       = 0;
	int32_t       vertexOffset     = 0;

	// --- Instancing (InstancedVoxelMesh, future: instanced Mesh) ---
	// Per-instance data lives in an SSBO referenced by `instanceBuffer`; the
	// item addresses into it via firstInstance/instanceCount. This is the line
	// that makes "millions of grass blades" fall out as a single vkCmdDraw.
	BufferHandle  instanceBuffer;       // valid for InstancedVoxelMesh
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

	// --- SkinnedMesh fields ---
	// Joints come from the RenderScene's per-frame joint arena. SceneExtractor
	// pushes the joint matrices in once per Component::SkinnedMesh, then emits
	// one RenderItem per primitive sharing the same (firstJoint, jointCount)
	// range. The technique uploads the whole arena to its per-frame SSBO and
	// the shader indexes via `joints[firstJoint + boneIdx]`.
	uint32_t      firstJoint       = 0;
	uint32_t      jointCount       = 0;
	glm::vec4     baseColorFactor  = glm::vec4(1.0f);

	// Per-primitive material binding (set 1: combined image+sampler for the
	// base-color texture). Borrowed from the asset's primitive — RenderItem
	// does NOT own the BindingTable. Valid for the duration of the frame
	// the item was emitted in (the asset lives at least that long, and the
	// per-frame descriptor set inside the BindingTable is selected at draw
	// time via GetSet(frameIndex)).
	BindingTable* materialBindings = nullptr;

	// Alpha policy lifted from the source material. The shader uses these
	// to gate `discard` (Mask + Blend honor cutoff; Opaque skips the test).
	// Encoded as raw uint8 because RenderItem stays POD; the shader maps
	// 0=Opaque, 1=Mask, 2=Blend. doubleSided selects the no-cull pipeline.
	float         alphaCutoff      = 0.5f;
	uint8_t       alphaMode        = 0;     // 0=Opaque, 1=Mask, 2=Blend
	bool          doubleSided      = false;
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
void DrawSkinnedMeshItem    (const PassContext& ctx, const RenderItem& item, const RenderGraph& graph);
