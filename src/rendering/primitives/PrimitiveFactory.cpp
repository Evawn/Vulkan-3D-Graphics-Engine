#include "PrimitiveFactory.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

	// Material indices used by the island palette. Index 0 is reserved (empty).
	constexpr uint8_t MAT_STONE = 1;
	constexpr uint8_t MAT_DIRT  = 2;
	constexpr uint8_t MAT_GRASS = 3;
	constexpr uint8_t MAT_SAND  = 4;

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
	// Single-shade per material; later iterations can perturb per-voxel for
	// dithered ground variation.
	std::array<uint8_t, 256 * 4> BuildIslandPalette() {
		std::array<uint8_t, 256 * 4> pal{};
		auto put = [&](uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
			pal[idx * 4 + 0] = r;
			pal[idx * 4 + 1] = g;
			pal[idx * 4 + 2] = b;
			pal[idx * 4 + 3] = 255;
		};
		put(MAT_STONE,  90,  90,  95);
		put(MAT_DIRT,   92,  64,  40);
		put(MAT_GRASS,  60, 130,  50);
		put(MAT_SAND,  220, 200, 140);
		return pal;
	}

}  // namespace

VoxModel PrimitiveFactory::BakeIslandTerrain(const IslandTerrainConfig& cfg) {
	auto logger = spdlog::get("Render");

	// Pad up to a multiple of 8 per axis so the brickmap build pass tiles cleanly.
	const uint32_t sx = std::max(8u, RoundUpTo8(cfg.gridSize.x));
	const uint32_t sy = std::max(8u, RoundUpTo8(cfg.gridSize.y));
	const uint32_t sz = std::max(8u, RoundUpTo8(cfg.maxHeight));

	if (logger) {
		const uint64_t bytes = uint64_t(sx) * sy * sz;
		const double mb = bytes / (1024.0 * 1024.0);
		logger->info("BakeIslandTerrain: gridSize={}x{} maxHeight={} → padded {}x{}x{} ({:.1f} MB)",
		             cfg.gridSize.x, cfg.gridSize.y, cfg.maxHeight, sx, sy, sz, mb);
		if (mb > 512.0) {
			logger->warn("BakeIslandTerrain: dense volume is {:.1f} MB — large allocations ahead", mb);
		}
	}

	VoxModel model{};
	model.sizeX = sx;
	model.sizeY = sy;
	model.sizeZ = sz;
	model.volumeSize = glm::uvec3(sx, sy, sz);
	model.volume.assign(uint64_t(sx) * sy * sz, 0u);
	model.palette = BuildIslandPalette();

	// Heightmap pass — separate so we don't recompute fBm per Z step.
	std::vector<uint32_t> heightmap(uint64_t(sx) * sy, 0u);

	const float halfX = sx * 0.5f;
	const float halfY = sy * 0.5f;
	// Normalize radial distance against the smaller half-extent so non-square
	// grids still get a circular-ish island instead of an elongated one.
	const float halfMin = std::min(halfX, halfY);

	const float seaY    = cfg.seaLevel * static_cast<float>(sz);
	const float beachY  = (cfg.seaLevel + cfg.beachWidth) * static_cast<float>(sz);

	for (uint32_t y = 0; y < sy; ++y) {
		for (uint32_t x = 0; x < sx; ++x) {
			// fBm sampled in voxel space scaled by noiseScale.
			const float n = FBm2D(static_cast<float>(x) * cfg.noiseScale,
			                      static_cast<float>(y) * cfg.noiseScale, cfg);

			// Radial island falloff — 1 at center, 0 past islandRadius+islandFalloff.
			const float dx = (static_cast<float>(x) + 0.5f - halfX) / halfMin;
			const float dy = (static_cast<float>(y) + 0.5f - halfY) / halfMin;
			const float dist = std::sqrt(dx * dx + dy * dy);
			const float mask = 1.0f - Smoothstep(cfg.islandRadius, cfg.islandRadius + cfg.islandFalloff, dist);

			// Bias the heightmap so the island has a baseline above sea level
			// (otherwise low-noise patches in the island center would be water).
			// Centerline pushes height to ~maxHeight * (seaLevel + 0.4 * mask).
			const float baseline = (cfg.seaLevel + 0.05f) + 0.55f * mask;
			const float h01 = std::clamp(baseline + 0.35f * mask * (n - 0.5f), 0.0f, 1.0f);

			heightmap[uint64_t(y) * sx + x] = static_cast<uint32_t>(h01 * static_cast<float>(sz - 1));
		}
	}

	// Voxelize columns from heightmap.
	for (uint32_t y = 0; y < sy; ++y) {
		for (uint32_t x = 0; x < sx; ++x) {
			const uint32_t h = heightmap[uint64_t(y) * sx + x];
			const float    hf = static_cast<float>(h);

			// Surface that ends inside the beach band is "beachy" — fill the top
			// with sand instead of grass. Surfaces above are normal terrain.
			const bool isBeachColumn = (hf <= beachY);

			const uint32_t topZ = std::min<uint32_t>(h, sz - 1);
			for (uint32_t z = 0; z <= topZ; ++z) {
				if (static_cast<float>(z) < seaY) {
					// Below sea level — empty for v1 (water comes later).
					continue;
				}

				uint8_t mat;
				if (isBeachColumn) {
					mat = MAT_SAND;
				} else if (z == topZ) {
					mat = MAT_GRASS;
				} else if (z + 3u >= topZ) {
					mat = MAT_DIRT;
				} else {
					mat = MAT_STONE;
				}

				const uint64_t idx = (uint64_t(z) * sy + y) * sx + x;
				model.volume[idx] = mat;
			}
		}
	}

	if (logger) {
		uint64_t filled = 0;
		for (uint8_t v : model.volume) if (v != 0) ++filled;
		const double pct = 100.0 * static_cast<double>(filled) / static_cast<double>(model.volume.size());
		logger->info("BakeIslandTerrain: {} filled voxels ({:.2f}%)", filled, pct);
	}

	return model;
}
