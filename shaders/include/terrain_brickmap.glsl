// Terrain-brickmap shadow occupancy lookup.
//
// Used by the unified shadow query in substrate.glsl when the includer defines
// `SUBSTRATE_TERRAIN`. Standalone (without substrate.glsl), it can also serve
// as a reusable terrain-brickmap occupancy probe.
//
// REQUIRES the includer to:
//   1. Bind a terrain brickmap SSBO with the symbol `terrain`:
//        layout(std430, ...) readonly buffer TerrainBrickmapBuffer {
//            uint data[];
//        } terrain;
//   2. Provide an `ivec3 terrainOriginVoxel` value (push-constant or uniform)
//      holding the world-voxel offset of terrain voxel (0,0,0). Allows the
//      caller to anchor the terrain at any integer world-voxel position.
//
// Buffer layout matches src/rendering/voxel/Brickmap.h (the same buffer the
// terrain trace shader binds), so no new producer is needed.

#ifndef TERRAIN_BRICKMAP_GLSL_INCLUDED
#define TERRAIN_BRICKMAP_GLSL_INCLUDED

const uint kTerrainEmptyBrick    = 0xFFFFFFFFu;
const uint kTerrainHeaderWords   = 8u;     // matches BrickmapData layout
const uint kTerrainBrickVoxels   = 512u;   // 8^3
const uint kTerrainBrickWords    = 128u;   // 4 voxels packed per uint32

bool terrainSolidAtWorldVoxel(ivec3 worldVoxel) {
	// Convert world-voxel → terrain-local voxel coord.
	ivec3 v = worldVoxel - terrainOriginVoxel;

	ivec3 vs = ivec3(int(terrain.data[0]),
	                 int(terrain.data[1]),
	                 int(terrain.data[2]));
	if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, vs))) return false;

	int  bs = int(terrain.data[3]);
	ivec3 gd = ivec3(int(terrain.data[4]),
	                 int(terrain.data[5]),
	                 int(terrain.data[6]));

	ivec3 brick_cell = v / bs;
	ivec3 local      = v - brick_cell * bs;

	uint top_cells = uint(gd.x * gd.y * gd.z);
	uint grid_idx  = uint(brick_cell.x)
	               + uint(brick_cell.y) * uint(gd.x)
	               + uint(brick_cell.z) * uint(gd.x) * uint(gd.y);
	uint brick_index = terrain.data[kTerrainHeaderWords + grid_idx];
	if (brick_index == kTerrainEmptyBrick) return false;

	// Per-brick voxel addressing — same packing as brickVoxelMaterial in
	// brickmap_palette_trace.frag (linear = lz*64 + ly*8 + lx,
	// 4 voxels packed per uint32 word).
	uint linear   = uint(local.z) * 64u + uint(local.y) * 8u + uint(local.x);
	uint word_idx = linear >> 2;
	uint lane     = linear & 3u;
	uint word = terrain.data[kTerrainHeaderWords
	                         + top_cells
	                         + uint(brick_index) * kTerrainBrickWords
	                         + word_idx];
	uint mat = (word >> (lane * 8u)) & 0xFFu;
	return mat != 0u;
}

#endif  // TERRAIN_BRICKMAP_GLSL_INCLUDED
