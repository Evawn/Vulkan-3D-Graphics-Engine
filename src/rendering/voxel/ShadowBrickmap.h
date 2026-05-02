#pragma once

// ---- ShadowBrickmap ----
//
// A dedicated acceleration structure for sun-shadow occupancy queries. Per
// docs/SHADOW-BRICKS.md, shadow occupancy is its own concern, separate from
// the color/geometry brickmaps that drive the primary trace. This struct
// captures the world-space "is anything solid here?" question across both
// terrain (static) and foliage (per-frame animated) contributors, packed in
// a brick-grid layout the GLSL shadow query can walk with two-level DDA.
//
// Layout (uint32 words):
//   [0..7]                          BrickGrid header (volumeSize, brickSize,
//                                   gridDim, brickCount). Same shape as
//                                   BrickmapData and the foliage substrate.
//   [8..8+gridCells-1]              top-level grid: brick id or kEmptyBrick.
//   [staticPoolBase ..]             brickCount × 16 words: static occupancy
//                                   bitmask (terrain). Set once at bake;
//                                   never rewritten.
//   [dynamicPoolBase ..]            brickCount × 16 words: dynamic occupancy
//                                   bitmask (foliage). Cleared + rewritten
//                                   each frame by shadow_foliage_write.comp.
//
// The shadow query reads (static_bit | dynamic_bit) per voxel — see
// shaders/include/shadow_brickmap.glsl. The two-pool layout means:
//   - per-frame work touches only the dynamic pool (no need to "preserve"
//     static across the rewrite),
//   - static and dynamic never write-conflict (they live at disjoint word
//     offsets),
//   - debug toggles ("shadows from terrain only" / "from foliage only")
//     become a one-line shader change.
//
// Topology rebuild rules (rebuildTopology / docs/SHADOW-BRICKS.md §4):
//   - Static layer: any brick containing a solid terrain voxel.
//   - Dynamic layer: any brick intersected by an instance's MAXIMAL voxel
//     AABB (union over all animation frames). Foliage-only bricks have no
//     terrain bits set; mixed bricks (terrain surface + foliage above) hold
//     terrain bits in the static slot and foliage bits in the dynamic slot.
//
// Determinism: same (terrain, instance set) → byte-identical buffer.

#include "BrickGrid.h"
#include "Brickmap.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace ShadowBrickmap {

// Per-instance input the topology builder consumes. Decoupled from any
// technique's GPU instance struct so the shadow brickmap doesn't have to
// know what else lives alongside position+yaw.
struct InstanceInput {
	glm::ivec3 worldVoxelPos;   // integer voxel position in shadow-brickmap coords
	uint8_t    yawIdx;          // 0..3, Z-yaw (matches Substrate::InstanceInput)
};

// One per (instance, world-brick) pair. The per-frame foliage write
// compute pass dispatches one workgroup per entry; each workgroup is 8×8×8
// threads (one per voxel inside the brick).
//
// Layout matches shaders/shadow_foliage_write.comp's `InstanceBrickEntry`
// (std430, 32 bytes, ivec3 alignment = 16 bytes).
struct InstanceBrick {
	uint32_t instanceIdx;        // index into the foliage instance SSBO
	uint32_t brickId;            // dense brick index in the shadow brickmap
	uint32_t _pad0;
	uint32_t _pad1;
	int32_t  brickWorldVoxelX;   // world-voxel coord of the brick's lo corner
	int32_t  brickWorldVoxelY;
	int32_t  brickWorldVoxelZ;
	int32_t  _pad2;
};
static_assert(sizeof(InstanceBrick) == 32,
	"InstanceBrick must stay 32 bytes (std430 alignment, mirror of GLSL InstanceBrickEntry)");

struct Build {
	BrickGrid::Header header;            // gridDim, originVoxel, brickCount
	std::vector<uint32_t> data;          // header + top grid + static pool + dynamic pool

	uint32_t topGridBase        = 0;     // = BrickGrid::kHeaderWords
	uint32_t staticPoolBase     = 0;     // word offset of static brick pool
	uint32_t dynamicPoolBase    = 0;     // word offset of dynamic brick pool

	// Build-time artifacts consumed by the per-frame foliage write compute.
	// Empty when no foliage instances were passed.
	std::vector<InstanceBrick> instanceBricks;

	uint64_t ByteSize() const {
		return static_cast<uint64_t>(data.size()) * sizeof(uint32_t);
	}
	uint64_t InstanceBricksBytes() const {
		return static_cast<uint64_t>(instanceBricks.size()) * sizeof(InstanceBrick);
	}
};

// Build the shadow brickmap from a freshly-baked terrain brickmap and the
// current instance set. Both inputs are optional (empty terrain or empty
// instance set produces a sparse brickmap with the available layer only).
//
//   `terrain`           — source of static occupancy. Read via the same CPU
//                         probe used by the host today. The terrain's
//                         originVoxel sets the shadow brickmap's anchor:
//                         shadow voxel (0,0,0) coincides with terrain voxel
//                         (0,0,0) coincides with world voxel
//                         terrain.originVoxel.
//   `instances`         — foliage instances. Each instance contributes
//                         the bricks intersected by its MAXIMAL voxel AABB
//                         (yaw-rotated asset bounds, translated by
//                         worldVoxelPos). Animation does not change topology.
//   `assetSize`         — voxel extent of the foliage asset (16x32x16 for
//                         v1 grass). The maximal AABB is the asset's full
//                         box; per-frame painted-bounds shrinkage is
//                         ignored at the topology level (the bits not set
//                         per frame contribute zero to the dynamic pool).
//
// Returns a Build whose `data` is ready for graph upload, plus
// `instanceBricks` for the foliage write compute's dispatch shape.
Build BuildShadowBrickmap(const BrickmapData&         terrain,
                          const InstanceInput*        instances,
                          uint32_t                    instanceCount,
                          glm::uvec3                  assetSize);

// Compute an upper-bound size in words for the shadow brickmap buffer
// given the parameters that drive its size. Lets the technique allocate a
// Persistent buffer at RegisterPasses time without first running the build.
//
// Bound rules:
//   - gridDim worst case = ceil(spanXYZ / brickSize), where spanXYZ is the
//     terrain volume size (instances are constrained to terrain footprint).
//     Foliage instances may push out the AABB by ±max(asset.x, asset.y) on
//     each horizontal axis under yaw, plus +asset.z on Z; the bound adds
//     that overhang on every face.
//   - brickCount worst case = gridDim.x * y * z (every brick populated).
//   - Brick payload: kBitmaskWordsPerBrick × 2 (static + dynamic).
uint32_t UpperBoundWords(glm::uvec3 terrainVolumeSize,
                         glm::uvec3 assetSize,
                         uint32_t   instanceCount);

// Upper bound on instanceBricks count for a given instance population.
// Each instance's maximal AABB straddles at most
// ceil(asset.x/8 + 1) * ceil(asset.y/8 + 1) * ceil(asset.z/8 + 1) bricks
// (the +1 accounts for one-brick straddle along each axis).
uint32_t UpperBoundInstanceBricks(uint32_t   instanceCount,
                                  glm::uvec3 assetSize);

}  // namespace ShadowBrickmap
