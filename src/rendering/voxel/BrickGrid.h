#pragma once

#include <glm/glm.hpp>
#include <cstdint>

// ---- BrickGrid ----
//
// Shared layout skeleton for brick-grid acceleration structures. All brick
// grids in this engine — palette brickmap (terrain color), shadow occupancy
// brickmap (shadow trace), and the substrate's foliage CSR — share the same
// 8^3 brick stride, the same 8-word header shape, and the same empty-brick
// sentinel. This file owns those constants and a few arithmetic helpers so
// they can't drift between producers and consumers.
//
// Header layout (uint32 words):
//   [0..2]  volumeSize.xyz       (padded voxel extent, multiple of brickSize)
//   [3]     brickSize            (always 8)
//   [4..6]  gridDim.xyz          (= volumeSize / brickSize)
//   [7]     brickCount           (number of populated bricks)
//
// The top-level grid (gridDim.x*y*z words of brick id or kEmptyBrick) starts
// at word offset 8. The brick payload pool starts after the grid; its
// per-brick word count is payload-specific (palette = 128, shadow = 16+16).

namespace BrickGrid {

constexpr uint32_t kBrickSize    = 8;
constexpr uint32_t kEmptyBrick   = 0xFFFFFFFFu;
constexpr uint32_t kHeaderWords  = 8;

// Voxels per brick = 512.
constexpr uint32_t kVoxelsPerBrick = kBrickSize * kBrickSize * kBrickSize;

// One uint32 packs 32 occupancy bits → 16 words per shadow brick.
constexpr uint32_t kBitmaskWordsPerBrick = kVoxelsPerBrick / 32u;

// One uint32 packs 4 material-byte voxels → 128 words per palette brick.
constexpr uint32_t kPaletteWordsPerBrick = kVoxelsPerBrick / 4u;

// Header values shared by every brick-grid producer. Read by the C++ host
// at upload time; the GLSL side reads the same fields out of the SSBO via
// brick_grid.glsl helpers.
struct Header {
	glm::uvec3 volumeSize  = glm::uvec3(0);  // padded voxel extent (multiple of brickSize)
	glm::uvec3 gridDim     = glm::uvec3(0);  // bricks per axis = volumeSize / brickSize
	glm::ivec3 originVoxel = glm::ivec3(0);  // world-voxel offset of voxel (0,0,0)
	uint32_t   brickCount  = 0;              // populated bricks
};

// Floor-divide a signed integer by `kBrickSize`. C++'s `/` truncates toward
// zero, which gives the wrong rounding for negative numerators (and the
// substrate AABB can extend negative on X/Y under yaw 180°/270°). True floor.
inline int FloorDivBrick(int v) {
	return (v >= 0) ? (v / static_cast<int>(kBrickSize))
	                : -(((-v) + static_cast<int>(kBrickSize) - 1) / static_cast<int>(kBrickSize));
}
inline int CeilDivBrick(int v) {
	return (v >= 0) ? ((v + static_cast<int>(kBrickSize) - 1) / static_cast<int>(kBrickSize))
	                : -((-v) / static_cast<int>(kBrickSize));
}

// Linear voxel index inside a brick (8x8x8). Matches the GLSL convention used
// across every brick-grid consumer: linear = lz*64 + ly*8 + lx.
inline uint32_t BrickVoxelLinear(glm::ivec3 local) {
	return static_cast<uint32_t>(local.z) * 64u
	     + static_cast<uint32_t>(local.y) * 8u
	     + static_cast<uint32_t>(local.x);
}

// Linear top-grid cell index. Matches every consumer's gridIdx packing:
// idx = bx + by*gridDim.x + bz*gridDim.x*gridDim.y.
inline uint32_t TopGridIndex(glm::ivec3 brickCoord, glm::uvec3 gridDim) {
	return static_cast<uint32_t>(brickCoord.x)
	     + static_cast<uint32_t>(brickCoord.y) * gridDim.x
	     + static_cast<uint32_t>(brickCoord.z) * gridDim.x * gridDim.y;
}

}  // namespace BrickGrid
