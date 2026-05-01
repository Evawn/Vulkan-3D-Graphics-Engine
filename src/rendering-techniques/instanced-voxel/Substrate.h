#pragma once

// ---- World-grid occupancy substrate (foliage layer, v1) ----
//
// CPU-side builder for the per-brick instance overlap index that the
// InstancedVoxelTechnique's shadow query (shaders/include/substrate.glsl)
// walks. See VISION.md §3 and LIGHTING.md §3 for the architectural framing;
// this header is the v1 implementation, scoped to the foliage contribution
// only. The static (terrain) and dynamic (water) contributions are sibling
// layers added in later milestones.
//
// Lifetime: built host-side once per `RebuildInstanceData`. The same
// determinism that drives the per-instance grid (RNG seeded by `0xC0FFEE`)
// drives this build, so re-running it produces an identical buffer.
//
// Forward-look: this lives next to the foliage technique because the
// substrate currently only tracks foliage. When Milestone E adds the
// terrain contribution, the *type* graduates to `src/rendering/voxel/`
// (alongside Brickmap.h) and gains a `static_brick_payload` slot. The
// query interface in substrate.glsl stays put — only the inputs grow.

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace InstancedVoxel {

// Brick edge length, in world voxels. Matches the static brickmap pillar so
// Milestone E can plug terrain bricks into the same coordinate system without
// translation.
constexpr uint32_t kSubstrateBrickSize  = 8;
// Sentinel stored in the top-level grid for cells with no contribution from
// any pillar. The same sentinel the static brickmap uses so the layouts are
// drop-in compatible.
constexpr uint32_t kSubstrateEmptyBrick = 0xFFFFFFFFu;

// Header word count in the packed buffer. Layout (uint32 words):
//   [0..2] gridDim.xyz                  — bricks per axis
//   [3]    brickSize                    — = kSubstrateBrickSize
//   [4..6] originVoxel.xyz (signed)     — cloud-local voxel offset of brick (0,0,0)
//   [7]    brickCount                   — number of populated bricks
constexpr uint32_t kSubstrateHeaderWords = 8;

// Per-instance input the substrate consumes. Decoupled from `GpuInstance` so
// the substrate doesn't have to know about anything else stored alongside
// position+yaw (palette, animation phase, future per-instance signals). The
// caller reads the relevant fields out of its instance source-of-truth.
struct SubstrateInstanceInput {
	glm::ivec3 cloudVoxelPos;   // integer voxel position in cloud-local space
	uint8_t    yawIdx;          // 0..3, Z-yaw enumeration (matches GpuInstance::yawIdx)
};

// Output of a build: the originating dimensions plus a packed std430 buffer
// ready for `RenderGraph::UploadBufferData`. The buffer's actual byte size
// (data.size() × 4) may be smaller than the upper bound the technique
// allocated; the shader honours `brickCount` from the header to know where
// the CSR ends.
struct SubstrateBuild {
	glm::ivec3            originVoxel = glm::ivec3(0);   // brick(0,0,0) anchor in cloud voxels
	glm::uvec3            gridDim     = glm::uvec3(0);   // bricks per axis
	uint32_t              brickCount  = 0;               // populated bricks
	std::vector<uint32_t> data;                          // packed buffer, std430
};

// Compute an upper-bound byte size for the substrate buffer given the
// inputs the technique controls. Lets the technique allocate a Persistent
// buffer at `RegisterPasses` time without first running the build.
//
// Bound rules:
//   - gridDim worst case = (footprint + 2*pad) / brickSize, where footprint
//     is `gridDim_blades * pitch_voxels` per axis, and `pad` accounts for
//     yaw-90°/180°/270° overhang of `max(asset.x, asset.y)` voxels.
//   - brickCount worst case = gridDim.x * y * z (every brick populated).
//   - CSR entries worst case = instanceCount * maxBricksPerInstance, where
//     maxBricksPerInstance = ceil(asset.x/8 + 1) * ceil(asset.y/8 + 1) *
//     ceil(asset.z/8 + 1) — small for grass-sized assets.
uint32_t SubstrateUpperBoundWords(uint32_t  instanceCount,
                                  glm::uvec3 assetSize,
                                  uint32_t  bladeGridDim,
                                  uint32_t  bladePitchVoxels);

// Build the foliage substrate from a quantized instance set. Called from
// `RebuildInstanceData`. Determinism: same inputs → byte-identical output.
SubstrateBuild BuildFoliageSubstrate(const SubstrateInstanceInput* instances,
                                     uint32_t                       instanceCount,
                                     glm::uvec3                     assetSize);

}  // namespace InstancedVoxel
