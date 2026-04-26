#pragma once

#include "VoxLoader.h"

#include <glm/glm.hpp>
#include <cstdint>

// ---- PrimitiveFactory ----
//
// Engine-level voxel-asset producers. Each Bake* function takes a config struct
// and returns a fully-populated VoxModel — same shape VoxLoader emits — so the
// output plugs straight into AssetRegistry::ReplaceVoxelVolume / RegisterVoxelVolume
// without any new upload plumbing.
//
// Bake* functions are pure / threadsafe (no globals, no graph access). v1 is
// CPU-only; the foliage workflow's GPU bake will land alongside as a parallel
// API (BakeIslandTerrainGPU(cfg, RenderGraph&, AssetID target) etc.) without
// breaking callers — the contract is "produce voxel asset bytes for a config."
//
// Engine convention reminder: Z is up. gridSize is (X, Y) horizontal voxels;
// maxHeight is the Z extent.

struct IslandTerrainConfig {
	glm::uvec2 gridSize       = glm::uvec2(1024, 1024);   // horizontal extent (X, Y) in voxels
	uint32_t   maxHeight      = 128;                      // vertical (Z) extent in voxels

	// fBm noise — sampled at island-relative XY coords. noiseScale is the
	// frequency of the lowest octave in cycles/voxel; higher = bumpier.
	float      noiseScale     = 0.008f;
	int        octaves        = 5;
	float      lacunarity     = 2.0f;
	float      gain           = 0.5f;

	// Island falloff — radial smoothstep applied to the heightmap. Both values
	// are normalized to the half-extent of the grid: 0 = center, 1 = corner.
	// At distFromCenter < islandRadius the heightmap is unattenuated; over the
	// range [islandRadius, islandRadius + islandFalloff] it decays to zero.
	float      islandRadius   = 0.42f;
	float      islandFalloff  = 0.28f;

	// Sea level — Y voxels below seaLevel * maxHeight are empty (no water for v1).
	// Beach width is the band above sea level rendered as sand instead of grass.
	float      seaLevel       = 0.20f;
	float      beachWidth     = 0.04f;

	uint32_t   seed           = 1337;
};

namespace PrimitiveFactory {

	// CPU bake — run on the calling thread. Returns a VoxModel whose volume
	// bytes + palette can be moved straight into AssetRegistry::ReplaceVoxelVolume.
	// volumeSize is per-axis padded up to a multiple of 8 to match the brickmap
	// build pass's brick size.
	VoxModel BakeIslandTerrain(const IslandTerrainConfig& cfg);

}
