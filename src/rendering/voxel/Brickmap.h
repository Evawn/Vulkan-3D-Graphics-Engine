#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

// ---- BrickmapData ----
//
// CPU-side, GPU-layout-compatible brickmap. The byte layout of `data` matches
// the std430 buffer that brickmap_palette_trace.frag binds at set=0 binding=0,
// so a finished BrickmapData uploads with a single staging copy — no
// reformatting, no compute build pass.
//
// Layout (uint32 words):
//   [0..7]            header
//                       [0..2] volume size in voxels (vs.x, vs.y, vs.z)
//                       [3]    brick edge length (always 8 today)
//                       [4..6] grid dimension in bricks (gd.x, gd.y, gd.z)
//                       [7]    brick_count — number of populated bricks
//   [8 .. 8+gc-1]     top-level grid: one uint32 per 8^3 cell, either a brick
//                     index into the pool or 0xFFFFFFFF for empty.
//   [8+gc ..]         brick pool: brick_count * 128 uint32 words. Each brick
//                     packs 8x8x8 voxels at 4 voxels per word
//                     (linear = lz*64 + ly*8 + lx, byte_lane = linear % 4).
//
// Producers (e.g. PrimitiveFactory::BakeIslandTerrainBrickmap) emit `data`
// directly in this layout. Consumers upload it verbatim into the technique's
// brickmap storage buffer.

struct BrickmapData {
	glm::uvec3 volumeSize = glm::uvec3(0);   // padded voxel extent (multiple of brickSize per axis)
	glm::uvec3 gridDim    = glm::uvec3(0);   // bricks per axis = volumeSize / brickSize
	uint32_t   brickSize  = 8;
	uint32_t   brickCount = 0;               // populated bricks; mirrors data[7]

	// World-voxel coords of voxel (0,0,0) of this brickmap. The standalone
	// brickmap-palette technique centers its volume at world origin and ignores
	// this; the CombinedRenderer pins the terrain to a known world-voxel anchor
	// (so the shadow substrate can address terrain bricks via shared world-voxel
	// coords). Default zero — pre-existing producers don't have to set it.
	glm::ivec3 originVoxel = glm::ivec3(0);

	// Single contiguous buffer matching the GPU layout described above. Size in
	// uint32 words = 8 + gridDim.x*gridDim.y*gridDim.z + brickCount * 128.
	std::vector<uint32_t> data;

	// RGBA palette — same shape as VoxModel::palette so PaletteResource can
	// consume it directly.
	std::array<uint8_t, 256 * 4> palette{};

	// Convenience: byte size of `data` for upload sizing.
	uint64_t ByteSize() const {
		return static_cast<uint64_t>(data.size()) * sizeof(uint32_t);
	}
};
