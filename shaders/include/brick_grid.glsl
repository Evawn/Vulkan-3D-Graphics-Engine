// Shared brick-grid layout helpers. Every brick-grid consumer (terrain
// palette brickmap, shadow occupancy brickmap, substrate foliage CSR)
// shares an 8^3 brick stride and the same 8-word header shape. Centralizing
// the constants and arithmetic here keeps producers (C++ in
// src/rendering/voxel/BrickGrid.h) and consumers (this file's includers)
// from drifting.
//
// Header layout (uint32 words), mirrored across every consumer:
//   [0..2]  volumeSize.xyz       (padded voxel extent)
//   [3]     brickSize            (always 8)
//   [4..6]  gridDim.xyz          (= volumeSize / brickSize)
//   [7]     brickCount           (populated brick count)
//
// This file does NOT bind any SSBO. It exposes pure-arithmetic helpers
// over `ivec3` brick/voxel coords. Consumers do their own SSBO reads;
// the helpers below let those reads be expressed identically across
// brickmap variants.

#ifndef BRICK_GRID_GLSL_INCLUDED
#define BRICK_GRID_GLSL_INCLUDED

const uint kBrickSize           = 8u;
const uint kBrickSizeShift      = 3u;        // log2(kBrickSize) — used for >> 3
const uint kEmptyBrick          = 0xFFFFFFFFu;
const uint kBrickHeaderWords    = 8u;
const uint kVoxelsPerBrick      = 512u;      // 8^3
const uint kBitmaskWordsPerBrick = 16u;      // 512 bits / 32 bits-per-uint
const uint kPaletteWordsPerBrick = 128u;     // 512 voxels / 4 voxels-per-uint

// Floor-divide an ivec3 by 8 (signed-correct via arithmetic shift). The
// substrate's AABB can sit at negative origins under yaw 180°/270°, and
// shadow rays that escape the brickmap negative-x can hit -1 brickCoords
// before the bounds check rejects them. Using `/ 8` truncates toward zero
// and would mis-round negatives; `>> 3` on int does the right thing on
// every Vulkan-class GPU.
ivec3 voxelToBrickCoord(ivec3 voxel) {
    return ivec3(voxel.x >> int(kBrickSizeShift),
                 voxel.y >> int(kBrickSizeShift),
                 voxel.z >> int(kBrickSizeShift));
}

// Same floor-divide, but for a voxel coord that's already been shifted into
// some grid-local frame (i.e. the producer subtracted originVoxel before
// passing). Identical math; named for readability at call sites that have
// already done the shift.
ivec3 voxelToBrickCoordLocal(ivec3 voxelLocal) {
    return ivec3(voxelLocal.x >> int(kBrickSizeShift),
                 voxelLocal.y >> int(kBrickSizeShift),
                 voxelLocal.z >> int(kBrickSizeShift));
}

// Linear top-grid cell index. Matches BrickGrid::TopGridIndex on the C++
// side. Caller must have already bounds-checked brickCoord against gridDim.
uint topGridIndex(ivec3 brickCoord, ivec3 gridDim) {
    return uint(brickCoord.x)
         + uint(brickCoord.y) * uint(gridDim.x)
         + uint(brickCoord.z) * uint(gridDim.x) * uint(gridDim.y);
}

// Linear voxel index inside a brick. Matches every consumer's packing
// convention: linear = lz*64 + ly*8 + lx. Caller bounds-checks `local`
// to [0, 8) per-axis.
uint brickVoxelLinear(ivec3 local) {
    return uint(local.z) * 64u + uint(local.y) * 8u + uint(local.x);
}

#endif  // BRICK_GRID_GLSL_INCLUDED
