#pragma once

// ---- World-grid occupancy substrate ----
//
// CPU-side builder for the per-brick instance overlap index that the
// substrate's GLSL shadow query (shaders/include/substrate.glsl) walks.
// See VISION.md §3 and LIGHTING.md §3 for the architectural framing.
//
// Layered contributions per VISION.md §3.2:
//   - **Foliage** (v1, this file): per-brick instance overlap CSR. The foliage
//     cloud's quantized-instance set is reduced to "which instances touch this
//     brick", and the GLSL query ORs each touching instance's per-frame
//     bitmask into a per-brick scratch test at brick entry.
//   - **Static** (Milestone E): the terrain brickmap pillar's existing brick
//     payload. The shadow query walks the bound terrain BrickmapBuffer in
//     lockstep with the substrate DDA — see shaders/include/terrain_brickmap.glsl.
//     No data fold here; the substrate stays foliage-shaped, and the GLSL inner
//     loop tests both layers per voxel.
//   - **Dynamic** (water/particles, future): a sibling layer; the GLSL query
//     ORs it in alongside foliage and static.
//
// Lifetime: built host-side once per substrate-affecting change (foliage
// instance edit, terrain bake, etc). Determinism: same inputs → byte-
// identical output buffer.

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Substrate {

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

// Per-instance input the substrate consumes. Decoupled from any technique's
// instance struct so the substrate doesn't have to know about anything else
// stored alongside position+yaw (palette, animation phase, future per-
// instance signals). The caller reads the relevant fields out of its
// instance source-of-truth.
struct InstanceInput {
	glm::ivec3 cloudVoxelPos;   // integer voxel position in cloud-local space
	uint8_t    yawIdx;          // 0..3, Z-yaw enumeration
};

// Output of a build: the originating dimensions plus a packed std430 buffer
// ready for `RenderGraph::UploadBufferData`. The buffer's actual byte size
// (data.size() × 4) may be smaller than the upper bound the technique
// allocated; the shader honours `brickCount` from the header to know where
// the CSR ends.
struct Build {
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
uint32_t UpperBoundWords(uint32_t  instanceCount,
                         glm::uvec3 assetSize,
                         uint32_t  bladeGridDim,
                         uint32_t  bladePitchVoxels);

// Build the foliage layer of the substrate from a quantized instance set.
// Determinism: same inputs → byte-identical output buffer.
Build BuildFoliage(const InstanceInput* instances,
                   uint32_t              instanceCount,
                   glm::uvec3            assetSize);

}  // namespace Substrate
