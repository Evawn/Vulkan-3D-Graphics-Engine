#include "PrimitiveFactory.h"
#include "DefaultPalette.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

	// Local aliases for the shared terrain palette indices defined in
	// PrimitiveFactory.h. Same values; the header surface lets foliage-
	// placement code consult them without duplicating the constants.
	constexpr uint8_t MAT_STONE = TerrainMaterials::Stone;
	constexpr uint8_t MAT_DIRT  = TerrainMaterials::Dirt;
	constexpr uint8_t MAT_GRASS = TerrainMaterials::Grass;
	constexpr uint8_t MAT_SAND  = TerrainMaterials::Sand;

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
	// Starts from the engine default (BuildDefaultPalette) so indices the
	// terrain doesn't claim (5 = sod orange, 64..95 = HSV green band)
	// remain populated for the foliage shader's lookups. See
	// docs/COMBINED-FOLIAGE-BLACK-BUG.md for the failure mode that comes
	// from leaving those entries zeroed.
	std::array<uint8_t, 256 * 4> BuildIslandPalette() {
		auto pal = BuildDefaultPalette();
		auto put = [&](uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
			pal[idx * 4 + 0] = r;
			pal[idx * 4 + 1] = g;
			pal[idx * 4 + 2] = b;
			pal[idx * 4 + 3] = 255;
		};
		// Override the default's shape colors at terrain material slots.
		// Indices 5..9 keep their default values so foliage / other
		// callers that overlay onto a fresh terrain palette don't lose
		// the rest of the engine's color set.
		put(MAT_STONE,  90,  90,  95);
		put(MAT_DIRT,   92,  64,  40);
		put(MAT_GRASS,  60, 130,  50);
		put(MAT_SAND,  220, 200, 140);
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

			const float baseline = (cfg.seaLevel + 0.05f) + 0.55f * mask;
			const float h01 = std::clamp(baseline + 0.35f * mask * (n - 0.5f), 0.0f, 1.0f);

			const uint32_t h = static_cast<uint32_t>(h01 * static_cast<float>(sz - 1));
			heightmap[uint64_t(y) * sx + x] = static_cast<uint16_t>(h);
			if (h > globalHmax) globalHmax = h;
		}
	}

	// Reserve a rough upper bound for the pool so we avoid mid-bake reallocations.
	// Surface band (seaY .. globalHmax) is everything that *might* contain
	// bricks; assume worst case all those bricks are populated.
	{
		const uint32_t bzLo = static_cast<uint32_t>(std::max(0.0f, std::floor(seaY))) / kBrickSize;
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

		// Whole-z-slab below sea is uniformly empty regardless of XY.
		const bool slabBelowSea = (static_cast<float>(zMaxInBrick) < seaY);

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
						const bool isBeachColumn = (static_cast<float>(h) <= beachY);

						const uint32_t zStart = z0;
						const uint32_t zEnd   = std::min<uint32_t>(z0 + kBrickSize - 1, topZ);
						if (zEnd < zStart) continue;
						for (uint32_t gz = zStart; gz <= zEnd; ++gz) {
							if (static_cast<float>(gz) < seaY) continue;

							uint8_t mat;
							if (isBeachColumn) {
								mat = MAT_SAND;
							} else if (gz == topZ) {
								mat = MAT_GRASS;
							} else if (gz + 3u >= topZ) {
								mat = MAT_DIRT;
							} else {
								mat = MAT_STONE;
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
