#pragma once

#include "Brickmap.h"

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

// Terrain material palette indices. Reserved low-numbered slots; the rest of
// the 256-entry palette is populated by BuildIslandPalette / BuildDefault
// Palette so foliage indices (5+) remain available. Surface-aware foliage
// placement consumes these to gate "is this column inland grass?" without
// re-deriving the threshold from elevation alone.
namespace TerrainMaterials {
	constexpr uint8_t Stone = 1;
	constexpr uint8_t Dirt  = 2;
	constexpr uint8_t Grass = 3;
	constexpr uint8_t Sand  = 4;
}

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

	// Domain warp — the radial distance fed into the falloff smoothstep is
	// perturbed by two decorrelated noise samples (one per axis) so the coast
	// becomes irregular: bays, peninsulas, no detached islets. Amp is in units
	// of the normalized half-extent (so 0.25 = up to a quarter of the radius
	// of distortion). Set amp=0 to recover a perfect circle.
	float      domainWarpFreq = 0.005f;
	float      domainWarpAmp  = 0.25f;

	// Sea level — Y voxels below seaLevel * maxHeight are empty (no water for v1).
	// Beach width is the band above sea level rendered as sand instead of grass.
	float      seaLevel       = 0.20f;
	float      beachWidth     = 0.04f;

	uint32_t   seed           = 1337;
};

namespace PrimitiveFactory {

	// CPU bake — run on the calling thread. Emits a finished brickmap directly
	// in the GPU layout: no dense intermediate volume is allocated, and the
	// GPU compute build pass is unnecessary. Heightmap-driven brick skipping
	// means CPU memory scales with the *populated* surface band, not with the
	// total bounding volume.
	//
	// volumeSize is per-axis padded up to a multiple of brickSize (8). Caller
	// uploads the returned `data` straight into the technique's brickmap
	// storage buffer and the `palette` straight into PaletteResource.
	BrickmapData BakeIslandTerrainBrickmap(const IslandTerrainConfig& cfg);

}
