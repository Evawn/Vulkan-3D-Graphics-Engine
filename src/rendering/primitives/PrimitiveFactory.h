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

// Terrain material palette layout. Each surface band (grass / sand / dirt /
// stone / subaqueous) occupies a contiguous run of palette slots so the bake
// can pick a per-voxel "stop" along the band and downstream consumers can ask
// "is this material in the grass band?" via the IsX predicates. Slots are
// claimed in the 10..25 range, leaving 0..9 and 26..255 free for primitives
// and foliage. Surface-aware foliage placement reads back surface material
// indices and uses IsGrass(...) to gate "is this column inland grass?"
// without re-deriving the threshold from elevation alone.
namespace TerrainMaterials {
	constexpr uint8_t GrassBase = 10; constexpr uint8_t GrassCount = 4;
	constexpr uint8_t SandBase  = 14; constexpr uint8_t SandCount  = 3;
	constexpr uint8_t DirtBase  = 17; constexpr uint8_t DirtCount  = 3;
	constexpr uint8_t StoneBase = 20; constexpr uint8_t StoneCount = 3;
	constexpr uint8_t SubaqBase = 23; constexpr uint8_t SubaqCount = 3;

	constexpr bool IsGrass(uint8_t m) { return m >= GrassBase && m < GrassBase + GrassCount; }
	constexpr bool IsSand (uint8_t m) { return m >= SandBase  && m < SandBase  + SandCount;  }
	constexpr bool IsSubaq(uint8_t m) { return m >= SubaqBase && m < SubaqBase + SubaqCount; }
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
	float      beachWidth     = 0.08f;

	// Per-voxel color jitter — amplitude (in band-parameter units, [0,1]) of
	// the noise nudge applied to each voxel's gradient-band lookup, so the
	// chosen palette stop varies voxel-to-voxel inside each band. Set to 0
	// for crisply banded surfaces; the default chunky look is around 0.15.
	float      colorJitter    = 0.15f;

	// Underwater band — voxels in the depth interval [seaLevel - underwater
	// MaxDepth, seaLevel] of an underwater column are voxelized with the
	// subaqueous gradient so the water shader can reveal silt-bottom near
	// shore. Deeper voxels are skipped (saves brick budget; the water plane
	// reads opaque past the shallow band anyway).
	float      underwaterMaxDepth = 24.0f;

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
