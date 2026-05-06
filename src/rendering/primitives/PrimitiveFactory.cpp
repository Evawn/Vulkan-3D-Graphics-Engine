#include "PrimitiveFactory.h"
#include "DefaultPalette.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

	inline uint32_t RoundUpTo8(uint32_t v) {
		return (v + 7u) & ~7u;
	}

	// 2D integer hash → uniform float in [0, 1). XOR-multiply mix; not crypto-grade
	// but plenty stable for value noise.
	inline float Hash2D(int32_t x, int32_t y, uint32_t seed) {
		uint32_t h = static_cast<uint32_t>(x) * 374761393u
		           + static_cast<uint32_t>(y) * 668265263u
		           + seed * 2654435761u;
		h ^= (h >> 13);
		h *= 1274126177u;
		h ^= (h >> 16);
		return static_cast<float>(h) * (1.0f / 4294967296.0f);
	}

	// 3D integer hash — same family as Hash2D with one extra prime so the
	// per-voxel color jitter doesn't repeat along Z slabs.
	inline float Hash3D(int32_t x, int32_t y, int32_t z, uint32_t seed) {
		uint32_t h = static_cast<uint32_t>(x) * 374761393u
		           + static_cast<uint32_t>(y) * 668265263u
		           + static_cast<uint32_t>(z) * 1610612741u
		           + seed * 2654435761u;
		h ^= (h >> 13);
		h *= 1274126177u;
		h ^= (h >> 16);
		return static_cast<float>(h) * (1.0f / 4294967296.0f);
	}

	// Quintic smoothstep — C2-continuous, gives noise its hand-tuned look.
	inline float Smoothstep5(float t) {
		return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
	}

	inline float Mix(float a, float b, float t) {
		return a + (b - a) * t;
	}

	// 2D value noise sampled at world-space coords. Returns [0, 1).
	float ValueNoise2D(float x, float y, uint32_t seed) {
		const float fx = std::floor(x);
		const float fy = std::floor(y);
		const int32_t ix = static_cast<int32_t>(fx);
		const int32_t iy = static_cast<int32_t>(fy);
		const float tx = Smoothstep5(x - fx);
		const float ty = Smoothstep5(y - fy);

		const float v00 = Hash2D(ix    , iy    , seed);
		const float v10 = Hash2D(ix + 1, iy    , seed);
		const float v01 = Hash2D(ix    , iy + 1, seed);
		const float v11 = Hash2D(ix + 1, iy + 1, seed);

		return Mix(Mix(v00, v10, tx), Mix(v01, v11, tx), ty);
	}

	// 3D value noise — 8-corner trilinear interpolation with quintic smoothing.
	// Sampled at integer voxel coords (no scaling) by the per-voxel color
	// jitter, which makes adjacent voxels effectively decorrelated (since
	// tx=ty=tz=0 at integer inputs and the result is just Hash3D of the
	// nearest lattice point). At fractional inputs the smoothing kicks in.
	float ValueNoise3D(float x, float y, float z, uint32_t seed) {
		const float fx = std::floor(x);
		const float fy = std::floor(y);
		const float fz = std::floor(z);
		const int32_t ix = static_cast<int32_t>(fx);
		const int32_t iy = static_cast<int32_t>(fy);
		const int32_t iz = static_cast<int32_t>(fz);
		const float tx = Smoothstep5(x - fx);
		const float ty = Smoothstep5(y - fy);
		const float tz = Smoothstep5(z - fz);

		const float v000 = Hash3D(ix    , iy    , iz    , seed);
		const float v100 = Hash3D(ix + 1, iy    , iz    , seed);
		const float v010 = Hash3D(ix    , iy + 1, iz    , seed);
		const float v110 = Hash3D(ix + 1, iy + 1, iz    , seed);
		const float v001 = Hash3D(ix    , iy    , iz + 1, seed);
		const float v101 = Hash3D(ix + 1, iy    , iz + 1, seed);
		const float v011 = Hash3D(ix    , iy + 1, iz + 1, seed);
		const float v111 = Hash3D(ix + 1, iy + 1, iz + 1, seed);

		const float a = Mix(Mix(v000, v100, tx), Mix(v010, v110, tx), ty);
		const float b = Mix(Mix(v001, v101, tx), Mix(v011, v111, tx), ty);
		return Mix(a, b, tz);
	}

	// Quantize t∈[0,1] into one of `count` band stops, return the palette
	// index `base + bucket`. Used by the per-voxel material picker to step
	// through a gradient (e.g. 4 grass stops from lush → alpine).
	inline uint8_t SelectBandStop(uint8_t base, uint8_t count, float t01) {
		const int n = static_cast<int>(count);
		int bucket = static_cast<int>(t01 * static_cast<float>(n));
		if (bucket < 0)        bucket = 0;
		else if (bucket >= n)  bucket = n - 1;
		return static_cast<uint8_t>(base + bucket);
	}

	// Fractional Brownian motion — N octaves of value noise summed with
	// geometric amplitude/frequency progression. Result normalized to [0, 1].
	float FBm2D(float x, float y, const IslandTerrainConfig& cfg) {
		float amp = 1.0f, freq = 1.0f, sum = 0.0f, weight = 0.0f;
		for (int o = 0; o < cfg.octaves; ++o) {
			sum    += amp * ValueNoise2D(x * freq, y * freq, cfg.seed + static_cast<uint32_t>(o));
			weight += amp;
			amp    *= cfg.gain;
			freq   *= cfg.lacunarity;
		}
		return weight > 0.0f ? (sum / weight) : 0.0f;
	}

	inline float Smoothstep(float edge0, float edge1, float x) {
		if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
		const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return Smoothstep5(t);
	}

	// Build the palette at runtime — derived (not user-configurable yet).
	// Starts from the engine default so unclaimed indices retain reasonable
	// defaults (the terrain bake never samples those slots, so the values
	// are only ever observed if other code paths reuse this palette).
	// Populates slots 10..25 with five gradient bands keyed by the
	// TerrainMaterials::*Base/*Count constants.
	std::array<uint8_t, 256 * 4> BuildIslandPalette() {
		auto pal = BuildDefaultPalette();
		auto put = [&](uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
			pal[idx * 4 + 0] = r;
			pal[idx * 4 + 1] = g;
			pal[idx * 4 + 2] = b;
			pal[idx * 4 + 3] = 255;
		};

		// Grass band (10..13) — lush near beach → dry/alpine near peak.
		put(TerrainMaterials::GrassBase + 0,  85, 150,  55);   // LUSH
		put(TerrainMaterials::GrassBase + 1,  60, 130,  50);   // STD (matches legacy)
		put(TerrainMaterials::GrassBase + 2, 110, 130,  60);   // DRY
		put(TerrainMaterials::GrassBase + 3, 130, 140, 120);   // ALPINE

		// Sand band (14..16) — wet at sea level → dry at beach top.
		put(TerrainMaterials::SandBase  + 0, 170, 150, 105);   // WET
		put(TerrainMaterials::SandBase  + 1, 220, 200, 140);   // BEACH (matches legacy)
		put(TerrainMaterials::SandBase  + 2, 235, 220, 175);   // DRY

		// Dirt band (17..19) — topsoil → loam → subsoil with depth.
		put(TerrainMaterials::DirtBase  + 0, 110,  72,  44);   // TOPSOIL (warm)
		put(TerrainMaterials::DirtBase  + 1,  92,  64,  40);   // LOAM (matches legacy)
		put(TerrainMaterials::DirtBase  + 2,  72,  60,  48);   // SUBSOIL (cool)

		// Stone band (20..22) — neutral → cool blue-gray + warm tan-gray.
		put(TerrainMaterials::StoneBase + 0,  90,  90,  95);   // BASE (matches legacy)
		put(TerrainMaterials::StoneBase + 1,  85,  95, 105);   // BLUE
		put(TerrainMaterials::StoneBase + 2, 110, 100,  85);   // TAN

		// Subaqueous band (23..25) — silty/algal near surface → dark deep.
		put(TerrainMaterials::SubaqBase + 0, 120, 130,  90);   // SHORE silt + algae
		put(TerrainMaterials::SubaqBase + 1,  70, 100, 110);   // MID muted blue-green
		put(TerrainMaterials::SubaqBase + 2,  35,  55,  75);   // DEEP dark blue-gray
		return pal;
	}

}  // namespace

BrickmapData PrimitiveFactory::BakeIslandTerrainBrickmap(const IslandTerrainConfig& cfg) {
	auto logger = spdlog::get("Render");

	// Pad up to a multiple of the brick edge so each brick maps onto a clean
	// 8^3 region. brickSize is fixed at 8 — matches the trace shader's layout.
	constexpr uint32_t kBrickSize = 8;
	const uint32_t sx = std::max(kBrickSize, RoundUpTo8(cfg.gridSize.x));
	const uint32_t sy = std::max(kBrickSize, RoundUpTo8(cfg.gridSize.y));
	const uint32_t sz = std::max(kBrickSize, RoundUpTo8(cfg.maxHeight));

	const uint32_t gdx = sx / kBrickSize;
	const uint32_t gdy = sy / kBrickSize;
	const uint32_t gdz = sz / kBrickSize;
	const uint64_t gridCells = uint64_t(gdx) * gdy * gdz;

	BrickmapData out{};
	out.volumeSize = glm::uvec3(sx, sy, sz);
	out.gridDim    = glm::uvec3(gdx, gdy, gdz);
	out.brickSize  = kBrickSize;
	out.brickCount = 0;
	out.palette    = BuildIslandPalette();

	// Header (8 words) + top grid (gridCells words). Pool entries appended as
	// non-empty bricks are discovered. Sentinel-fill the grid up front so any
	// brick we skip is implicitly marked empty.
	out.data.assign(8 + gridCells, 0xFFFFFFFFu);
	out.data[0] = sx;
	out.data[1] = sy;
	out.data[2] = sz;
	out.data[3] = kBrickSize;
	out.data[4] = gdx;
	out.data[5] = gdy;
	out.data[6] = gdz;
	out.data[7] = 0;  // brick_count, populated below

	// Heightmap — 2 bytes per XY column. uint16 caps at 65535, far above the
	// max-height slider's 512, so packed storage is safe and halves the only
	// allocation that scales with horizontal extent at large sizes (256 MB →
	// 128 MB at 8192^2).
	std::vector<uint16_t> heightmap(uint64_t(sx) * sy, 0u);

	const float halfX = sx * 0.5f;
	const float halfY = sy * 0.5f;
	// Normalize radial distance against the smaller half-extent so non-square
	// grids still get a circular-ish island instead of an elongated one.
	const float halfMin = std::min(halfX, halfY);

	const float seaY   = cfg.seaLevel * static_cast<float>(sz);
	const float beachY = (cfg.seaLevel + cfg.beachWidth) * static_cast<float>(sz);

	uint32_t globalHmax = 0;
	for (uint32_t y = 0; y < sy; ++y) {
		for (uint32_t x = 0; x < sx; ++x) {
			const float n = FBm2D(static_cast<float>(x) * cfg.noiseScale,
			                      static_cast<float>(y) * cfg.noiseScale, cfg);

			const float dx = (static_cast<float>(x) + 0.5f - halfX) / halfMin;
			const float dy = (static_cast<float>(y) + 0.5f - halfY) / halfMin;

			// Domain-warped distance: two decorrelated noise samples push the
			// (dx, dy) lookup point around in halfMin units before we measure
			// radial distance, deforming the level set into bays/peninsulas
			// without breaking the island into multiple blobs.
			const float wnx = ValueNoise2D(static_cast<float>(x) * cfg.domainWarpFreq,
			                               static_cast<float>(y) * cfg.domainWarpFreq,
			                               cfg.seed + 17u);
			const float wny = ValueNoise2D(static_cast<float>(x) * cfg.domainWarpFreq,
			                               static_cast<float>(y) * cfg.domainWarpFreq,
			                               cfg.seed + 31u);
			const float wx = (wnx - 0.5f) * 2.0f * cfg.domainWarpAmp;
			const float wy = (wny - 0.5f) * 2.0f * cfg.domainWarpAmp;
			const float wdx = dx + wx;
			const float wdy = dy + wy;
			const float dist = std::sqrt(wdx * wdx + wdy * wdy);
			const float mask = 1.0f - Smoothstep(cfg.islandRadius, cfg.islandRadius + cfg.islandFalloff, dist);

			// Baseline lerps from a submerged offshore floor (mask=0) to a
			// peak at mask=1. The "shoreline" is where baseline crosses
			// seaLevel, which falls inside the falloff zone naturally
			// instead of at the literal grid edge — gives the water plane
			// real water to show off, and the beach band lands at the
			// island's actual coast.
			const float baseline = (cfg.seaLevel - 0.15f) + 0.70f * mask;
			const float h01 = std::clamp(baseline + 0.35f * mask * (n - 0.5f), 0.0f, 1.0f);

			const uint32_t h = static_cast<uint32_t>(h01 * static_cast<float>(sz - 1));
			heightmap[uint64_t(y) * sx + x] = static_cast<uint16_t>(h);
			if (h > globalHmax) globalHmax = h;
		}
	}

	// Underwater band — voxels in [underwaterFloorY, seaY] of underwater
	// columns get rasterized with the subaqueous gradient so the water
	// shader can reveal silt-bottom near shore. Computed once; used both
	// for the reservation math and the per-brick / per-voxel skip guards.
	const float underwaterFloorY = std::max(0.0f, seaY - cfg.underwaterMaxDepth);

	// Reserve a rough upper bound for the pool so we avoid mid-bake reallocations.
	// Brick band (underwaterFloorY .. globalHmax) is everything that *might*
	// contain bricks; assume worst case all those bricks are populated.
	{
		const uint32_t bzLo = static_cast<uint32_t>(std::floor(underwaterFloorY)) / kBrickSize;
		const uint32_t bzHi = std::min(gdz, (globalHmax / kBrickSize) + 1);
		const uint64_t maxBricks = uint64_t(gdx) * gdy * (bzHi > bzLo ? (bzHi - bzLo) : 0);
		out.data.reserve(out.data.size() + maxBricks * 128);
	}

	// Per-brick scratch — 128 words = exactly one brick in the pool layout.
	uint32_t scratch[128];

	uint64_t skippedAbove = 0;
	uint64_t skippedBelow = 0;
	uint64_t skippedEmptyBand = 0;
	uint64_t emittedBricks = 0;

	for (uint32_t bz = 0; bz < gdz; ++bz) {
		const uint32_t z0 = bz * kBrickSize;
		const uint32_t zMaxInBrick = z0 + kBrickSize - 1;

		// Whole-z-slab below the underwater shallow band is uniformly empty
		// regardless of XY (deep ocean floor is intentionally not voxelized;
		// the water plane reads opaque past kUnderwaterMaxDepth).
		const bool slabBelowSea = (static_cast<float>(zMaxInBrick) < underwaterFloorY);

		for (uint32_t by = 0; by < gdy; ++by) {
			const uint32_t y0 = by * kBrickSize;
			for (uint32_t bx = 0; bx < gdx; ++bx) {
				const uint32_t x0 = bx * kBrickSize;
				const uint64_t gridIdx = uint64_t(bx)
				                       + uint64_t(by) * gdx
				                       + uint64_t(bz) * gdx * gdy;

				if (slabBelowSea) {
					++skippedBelow;
					continue;  // sentinel already in place
				}

				// Find min/max heights under this brick's 8x8 XY footprint.
				uint32_t hmin = UINT32_MAX;
				uint32_t hmax = 0;
				for (uint32_t ly = 0; ly < kBrickSize; ++ly) {
					const uint16_t* row = &heightmap[uint64_t(y0 + ly) * sx + x0];
					for (uint32_t lx = 0; lx < kBrickSize; ++lx) {
						const uint32_t h = row[lx];
						if (h < hmin) hmin = h;
						if (h > hmax) hmax = h;
					}
				}

				// Brick entirely above all surfaces under it → all voxels empty.
				if (z0 > hmax) {
					++skippedAbove;
					continue;
				}

				// Brick lies under terrain but the slice [seaY, hmax] doesn't
				// reach it: hmax < z0 already handled above. The opposite case
				// — z0 above globalHmax — is also folded into that test. Below
				// we fall through to per-voxel rasterization.
				(void)hmin;

				// Voxelize this brick into scratch.
				for (uint32_t i = 0; i < 128; ++i) scratch[i] = 0u;
				bool anyOccupied = false;

				for (uint32_t ly = 0; ly < kBrickSize; ++ly) {
					for (uint32_t lx = 0; lx < kBrickSize; ++lx) {
						const uint32_t h = heightmap[uint64_t(y0 + ly) * sx + (x0 + lx)];
						const uint32_t topZ = std::min<uint32_t>(h, sz - 1);
						const bool isUnderwaterColumn = (static_cast<float>(topZ) < seaY);
						const bool isBeachColumn      = !isUnderwaterColumn
						                                 && (static_cast<float>(h) <= beachY);

						const uint32_t zStart = z0;
						const uint32_t zEnd   = std::min<uint32_t>(z0 + kBrickSize - 1, topZ);
						if (zEnd < zStart) continue;
						for (uint32_t gz = zStart; gz <= zEnd; ++gz) {
							if (isUnderwaterColumn) {
								if (static_cast<float>(gz) < underwaterFloorY) continue;
							} else if (static_cast<float>(gz) < seaY) {
								continue;
							}

							// Per-voxel jitter — sampled at integer voxel coords for
							// fully decorrelated per-voxel hashes (the trilinear
							// blend collapses to Hash3D at integer inputs). Breaks
							// up horizontal striping across the gradient bands.
							const float jraw = ValueNoise3D(static_cast<float>(x0 + lx),
							                                static_cast<float>(y0 + ly),
							                                static_cast<float>(gz),
							                                cfg.seed + 101u);
							const float jitter = (jraw - 0.5f) * cfg.colorJitter;

							uint8_t mat;
							if (isUnderwaterColumn) {
								// Subaqueous: 0 just below sea (silt) → 1 deep (dark).
								const float depth = seaY - static_cast<float>(gz);
								const float t = (cfg.underwaterMaxDepth > 0.0f)
								                  ? depth / cfg.underwaterMaxDepth
								                  : 0.0f;
								mat = SelectBandStop(TerrainMaterials::SubaqBase,
								                     TerrainMaterials::SubaqCount,
								                     std::clamp(t + jitter, 0.0f, 1.0f));
							} else if (isBeachColumn) {
								// Sand: 0 wet at sea level → 1 dry at beach top.
								const float denom = std::max(1.0f, beachY - seaY);
								const float t = (static_cast<float>(gz) - seaY) / denom;
								mat = SelectBandStop(TerrainMaterials::SandBase,
								                     TerrainMaterials::SandCount,
								                     std::clamp(t + jitter, 0.0f, 1.0f));
							} else if (gz == topZ) {
								// Grass: lush near beach top → alpine near global peak.
								// Shared globalHmax reference keeps the vertical scale
								// consistent across all surface grass voxels.
								const float denom = std::max(1.0f, static_cast<float>(globalHmax) - beachY);
								const float t = (static_cast<float>(topZ) - beachY) / denom;
								mat = SelectBandStop(TerrainMaterials::GrassBase,
								                     TerrainMaterials::GrassCount,
								                     std::clamp(t + jitter, 0.0f, 1.0f));
							} else if (gz + 3u >= topZ) {
								// Dirt: 0 just below grass → 1 at the bottom of the
								// 3-voxel topsoil/loam/subsoil band.
								const float t = static_cast<float>(topZ - gz) / 3.0f;
								mat = SelectBandStop(TerrainMaterials::DirtBase,
								                     TerrainMaterials::DirtCount,
								                     std::clamp(t + jitter, 0.0f, 1.0f));
							} else {
								// Stone: deeper subsurface. ~16-voxel band depth for
								// the gradient parameter; cliff cuts beyond saturate
								// at the deepest stop. Wider jitter range here would
								// require a per-band knob; for now the shared
								// colorJitter applies uniformly.
								const float t = std::clamp(static_cast<float>(topZ - gz - 3u) / 16.0f,
								                           0.0f, 1.0f);
								mat = SelectBandStop(TerrainMaterials::StoneBase,
								                     TerrainMaterials::StoneCount,
								                     std::clamp(t + jitter, 0.0f, 1.0f));
							}

							const uint32_t lz = gz - z0;
							const uint32_t linear  = lz * 64u + ly * 8u + lx;
							const uint32_t word    = linear >> 2;
							const uint32_t laneBit = (linear & 3u) * 8u;
							scratch[word] |= static_cast<uint32_t>(mat) << laneBit;
							anyOccupied = true;
						}
					}
				}

				if (!anyOccupied) {
					++skippedEmptyBand;
					continue;
				}

				const uint32_t brickIdx = out.brickCount++;
				out.data[8 + gridIdx] = brickIdx;
				out.data.insert(out.data.end(), scratch, scratch + 128);
				++emittedBricks;
			}
		}
	}

	out.data[7] = out.brickCount;

	if (logger) {
		const uint64_t totalCells   = gridCells;
		const uint64_t totalBytes   = out.ByteSize();
		const double   mb           = totalBytes / (1024.0 * 1024.0);
		const double   denseMb      = uint64_t(sx) * sy * sz / (1024.0 * 1024.0);
		const double   sparsity     = totalCells > 0
		                            ? 100.0 * static_cast<double>(emittedBricks) / static_cast<double>(totalCells)
		                            : 0.0;
		logger->info(
			"BakeIslandTerrainBrickmap: {}x{}x{} → grid {}x{}x{} ({} cells), "
			"emitted {} bricks ({:.2f}%), skipped above={} below={} empty-band={}, "
			"buffer {:.2f} MB (vs dense {:.1f} MB)",
			sx, sy, sz, gdx, gdy, gdz, totalCells,
			emittedBricks, sparsity, skippedAbove, skippedBelow, skippedEmptyBand,
			mb, denseMb);
	}

	return out;
}
