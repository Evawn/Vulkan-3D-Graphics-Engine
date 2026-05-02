// Shadow occupancy brickmap — GLSL read interface.
//
// Companion to src/rendering/voxel/ShadowBrickmap.h. The includer #includes
// this file and gets:
//   - constants for the static / dynamic pool offsets,
//   - a per-voxel "is anything solid here?" lookup (combines static + dynamic),
//   - a per-brick brick-id lookup for the outer DDA in substrate.glsl.
//
// REQUIRES the includer to bind one SSBO with the symbol `shadowBM`:
//
//   layout(std430, ...) readonly buffer ShadowBrickmapBuffer {
//       uint data[];
//   } shadowBM;
//
// The buffer's layout matches ShadowBrickmap::Build::data exactly:
//   [0..7]                          BrickGrid header (volumeSize / brickSize /
//                                   gridDim / brickCount).
//   [topGridBase..]                 top-level grid: brick id or kEmptyBrick.
//   [staticPoolBase..]              brickCount × 16 words: terrain occupancy.
//   [dynamicPoolBase..]             brickCount × 16 words: foliage occupancy.

#ifndef SHADOW_BRICKMAP_GLSL_INCLUDED
#define SHADOW_BRICKMAP_GLSL_INCLUDED

#include "brick_grid.glsl"

// ---- Header accessors -------------------------------------------------------

struct ShadowLayout {
	ivec3 gridDim;
	ivec3 originVoxel;
	uint  brickCount;
	uint  topGridBase;
	uint  staticPoolBase;
	uint  dynamicPoolBase;
};

ShadowLayout shadowBrickmapLayout() {
	ShadowLayout L;
	L.gridDim     = ivec3(int(shadowBM.data[0]),
	                      int(shadowBM.data[1]),
	                      int(shadowBM.data[2]));
	// data[3] = brickSize (== kBrickSize, sanity).
	L.originVoxel = ivec3(int(shadowBM.data[4]),
	                      int(shadowBM.data[5]),
	                      int(shadowBM.data[6]));
	L.brickCount  = shadowBM.data[7];

	uint topCells = uint(L.gridDim.x * L.gridDim.y * L.gridDim.z);
	L.topGridBase     = kBrickHeaderWords;
	L.staticPoolBase  = L.topGridBase + topCells;
	L.dynamicPoolBase = L.staticPoolBase + L.brickCount * kBitmaskWordsPerBrick;
	return L;
}

// ---- Brick-id lookup --------------------------------------------------------

// Returns the brick id at `brickCoord` (shadow-brickmap-local) or
// kEmptyBrick if out of range or unallocated.
uint shadowBrickAtCell(ivec3 brickCoord, ShadowLayout L) {
	if (any(lessThan(brickCoord, ivec3(0))) ||
	    any(greaterThanEqual(brickCoord, L.gridDim))) return kEmptyBrick;
	uint gridIdx = topGridIndex(brickCoord, L.gridDim);
	return shadowBM.data[L.topGridBase + gridIdx];
}

// ---- Per-voxel occupancy ----------------------------------------------------

// `worldVoxel` is in shadow-brickmap world coords (i.e. shifted so that
// shadowBM.originVoxel maps to (0,0,0)). Caller does the subtract.
//
// Returns true if either the static or dynamic layer reports the voxel
// solid. Inner-DDA hot path; one SSBO read per layer when the brick is
// allocated, zero reads when the brick id was already cached as empty by
// the outer DDA caller.
bool shadowVoxelSolid(uint brickId, ivec3 brickLocal, ShadowLayout L) {
	uint bitLinear = brickVoxelLinear(brickLocal);
	uint wordIdx   = bitLinear >> 5;
	uint bitMask   = 1u << (bitLinear & 31u);
	uint sWord = shadowBM.data[L.staticPoolBase  + brickId * kBitmaskWordsPerBrick + wordIdx];
	uint dWord = shadowBM.data[L.dynamicPoolBase + brickId * kBitmaskWordsPerBrick + wordIdx];
	return ((sWord | dWord) & bitMask) != 0u;
}

#endif  // SHADOW_BRICKMAP_GLSL_INCLUDED
