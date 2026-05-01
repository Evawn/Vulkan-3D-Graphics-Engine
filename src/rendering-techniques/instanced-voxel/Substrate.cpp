#include "Substrate.h"

#include <algorithm>
#include <cassert>
#include <climits>

namespace InstancedVoxel {

namespace {

constexpr int kBrick = static_cast<int>(kSubstrateBrickSize);

// Floor-divide a signed integer by `kBrick`. C++'s `/` truncates toward zero,
// which gives the wrong rounding for negative numerators. The substrate AABB
// can extend negative on X/Y under yaw 180°/270°, so we need true floor.
inline int FloorDivBrick(int v) {
	return (v >= 0) ? (v / kBrick) : -(((-v) + kBrick - 1) / kBrick);
}
// Ceil-divide a signed integer by `kBrick`. Mirrors FloorDivBrick.
inline int CeilDivBrick(int v) {
	return (v >= 0) ? ((v + kBrick - 1) / kBrick) : -((-v) / kBrick);
}

// Per-instance voxel-AABB in cloud-local space, half-open [lo, hi). Yaw acts
// on the asset's [0, size) box; the result is still axis-aligned because yaw
// is one of {0°, 90°, 180°, 270°} (LIGHTING.md §2). Then we translate by the
// instance's integer voxel position.
struct VoxelAabb { glm::ivec3 lo, hi; };

VoxelAabb InstanceAabb(const SubstrateInstanceInput& inst, glm::uvec3 assetSize) {
	const int sx = static_cast<int>(assetSize.x);
	const int sy = static_cast<int>(assetSize.y);
	const int sz = static_cast<int>(assetSize.z);
	glm::ivec3 lo, hi;
	switch (inst.yawIdx & 0x3) {
		case 0: lo = {  0,    0,   0}; hi = { sx,  sy,  sz}; break;
		case 1: lo = {-sy,    0,   0}; hi = {  0,  sx,  sz}; break;
		case 2: lo = {-sx,  -sy,   0}; hi = {  0,   0,  sz}; break;
		case 3: lo = {  0,  -sx,   0}; hi = { sy,   0,  sz}; break;
	}
	return { lo + inst.cloudVoxelPos, hi + inst.cloudVoxelPos };
}

}  // namespace

uint32_t SubstrateUpperBoundWords(uint32_t   instanceCount,
                                  glm::uvec3 assetSize,
                                  uint32_t   bladeGridDim,
                                  uint32_t   bladePitchVoxels)
{
	// Cloud-local voxel footprint plus yaw overhang on BOTH sides:
	//   yaw 1/2 puts the blade's negative-side AABB at -max(asset.x, asset.y);
	//   yaw 0/3 extends positive-side by +max(asset.x, asset.y) past the
	//   blade's origin. spanX/Y therefore need 2× the per-axis overhang.
	const int gridSpan = static_cast<int>(bladeGridDim) *
	                     static_cast<int>(bladePitchVoxels);
	const int yawPad   = static_cast<int>(std::max(assetSize.x, assetSize.y));
	const int spanX = gridSpan + 2 * yawPad;
	const int spanY = gridSpan + 2 * yawPad;
	const int spanZ = static_cast<int>(assetSize.z);

	// Brick-align: ceil-divide spans, then count cells.
	const uint32_t gx = static_cast<uint32_t>(CeilDivBrick(spanX));
	const uint32_t gy = static_cast<uint32_t>(CeilDivBrick(spanY));
	const uint32_t gz = static_cast<uint32_t>(CeilDivBrick(spanZ));
	const uint32_t topGridCells = gx * gy * gz;

	// Worst-case bricks per instance: an asset cube can straddle one brick
	// per axis along each dimension, so the bound is the per-axis brick span
	// rounded up plus one (the straddle). For 16×32×16 grass with an 8-voxel
	// brick this is 3 × 5 × 3 = 45 — comfortably small.
	const auto axisSpanBricks = [](uint32_t s) {
		return (s + kSubstrateBrickSize - 1u) / kSubstrateBrickSize + 1u;
	};
	const uint32_t maxBricksPerInst = axisSpanBricks(assetSize.x) *
	                                  axisSpanBricks(assetSize.y) *
	                                  axisSpanBricks(assetSize.z);
	const uint32_t maxEntries = instanceCount * maxBricksPerInst;

	// Each populated brick contributes one CSR-offset entry; the offset
	// table also has a final sentinel slot (brickCount + 1).
	const uint32_t maxOffsets = topGridCells + 1u;

	return kSubstrateHeaderWords + topGridCells + maxOffsets + maxEntries;
}

SubstrateBuild BuildFoliageSubstrate(const SubstrateInstanceInput* instances,
                                     uint32_t                       instanceCount,
                                     glm::uvec3                     assetSize)
{
	SubstrateBuild out;

	// 1. Cloud-local voxel AABB across all instances.
	if (instanceCount == 0) {
		out.gridDim     = glm::uvec3(0);
		out.brickCount  = 0;
		out.originVoxel = glm::ivec3(0);
		out.data.assign(kSubstrateHeaderWords, 0u);
		out.data[3] = kSubstrateBrickSize;
		return out;
	}

	glm::ivec3 cloudLo( INT_MAX), cloudHi(INT_MIN);
	for (uint32_t i = 0; i < instanceCount; ++i) {
		const auto a = InstanceAabb(instances[i], assetSize);
		cloudLo = glm::min(cloudLo, a.lo);
		cloudHi = glm::max(cloudHi, a.hi);
	}

	// 2. Brick-align: origin = floor / 8 * 8; max = ceil / 8 * 8.
	const glm::ivec3 originVoxel(
		FloorDivBrick(cloudLo.x) * kBrick,
		FloorDivBrick(cloudLo.y) * kBrick,
		FloorDivBrick(cloudLo.z) * kBrick);
	const glm::ivec3 maxVoxel(
		CeilDivBrick(cloudHi.x) * kBrick,
		CeilDivBrick(cloudHi.y) * kBrick,
		CeilDivBrick(cloudHi.z) * kBrick);
	const glm::uvec3 gridDim(
		static_cast<uint32_t>((maxVoxel.x - originVoxel.x) / kBrick),
		static_cast<uint32_t>((maxVoxel.y - originVoxel.y) / kBrick),
		static_cast<uint32_t>((maxVoxel.z - originVoxel.z) / kBrick));
	const uint32_t topGridCells = gridDim.x * gridDim.y * gridDim.z;

	// 3. Collect (gridIdx, instanceIdx) pairs by walking each instance's
	// brick AABB. Each cell visited becomes one entry in the unsorted list.
	std::vector<std::pair<uint32_t, uint32_t>> entries;
	entries.reserve(static_cast<size_t>(instanceCount) * 4);
	for (uint32_t i = 0; i < instanceCount; ++i) {
		const auto a = InstanceAabb(instances[i], assetSize);
		// Brick coordinates within the substrate grid (0-based).
		const glm::ivec3 brickLo(
			(a.lo.x - originVoxel.x) / kBrick,
			(a.lo.y - originVoxel.y) / kBrick,
			(a.lo.z - originVoxel.z) / kBrick);
		const glm::ivec3 brickHi(
			(a.hi.x - 1 - originVoxel.x) / kBrick,
			(a.hi.y - 1 - originVoxel.y) / kBrick,
			(a.hi.z - 1 - originVoxel.z) / kBrick);
		for (int bz = brickLo.z; bz <= brickHi.z; ++bz)
		for (int by = brickLo.y; by <= brickHi.y; ++by)
		for (int bx = brickLo.x; bx <= brickHi.x; ++bx) {
			assert(bx >= 0 && by >= 0 && bz >= 0);
			assert(bx < static_cast<int>(gridDim.x) &&
			       by < static_cast<int>(gridDim.y) &&
			       bz < static_cast<int>(gridDim.z));
			const uint32_t gridIdx = static_cast<uint32_t>(bx)
			                       + static_cast<uint32_t>(by) * gridDim.x
			                       + static_cast<uint32_t>(bz) * gridDim.x * gridDim.y;
			entries.push_back({ gridIdx, i });
		}
	}

	// 4. Sort by (gridIdx, instanceIdx) — stable enough since instanceIdx
	// is the secondary key. Determinism falls out for free.
	std::sort(entries.begin(), entries.end());

	// 5. Build top_grid + foliage CSR. One pass over the sorted entries.
	std::vector<uint32_t> topGrid(topGridCells, kSubstrateEmptyBrick);
	std::vector<uint32_t> foliageInstances;
	foliageInstances.reserve(entries.size());
	std::vector<uint32_t> foliageOffsets;
	foliageOffsets.push_back(0u);

	uint32_t brickCount = 0;
	size_t   cur        = 0;
	while (cur < entries.size()) {
		const uint32_t gridIdx = entries[cur].first;
		topGrid[gridIdx] = brickCount;
		while (cur < entries.size() && entries[cur].first == gridIdx) {
			foliageInstances.push_back(entries[cur].second);
			++cur;
		}
		foliageOffsets.push_back(static_cast<uint32_t>(foliageInstances.size()));
		++brickCount;
	}

	// 6. Pack into the std430 buffer layout.
	out.originVoxel = originVoxel;
	out.gridDim     = gridDim;
	out.brickCount  = brickCount;
	out.data.reserve(kSubstrateHeaderWords + topGridCells +
	                 foliageOffsets.size() + foliageInstances.size());
	// Header
	out.data.push_back(gridDim.x);
	out.data.push_back(gridDim.y);
	out.data.push_back(gridDim.z);
	out.data.push_back(kSubstrateBrickSize);
	out.data.push_back(static_cast<uint32_t>(originVoxel.x));
	out.data.push_back(static_cast<uint32_t>(originVoxel.y));
	out.data.push_back(static_cast<uint32_t>(originVoxel.z));
	out.data.push_back(brickCount);
	// Top-level grid
	out.data.insert(out.data.end(), topGrid.begin(), topGrid.end());
	// Foliage CSR
	out.data.insert(out.data.end(), foliageOffsets.begin(), foliageOffsets.end());
	out.data.insert(out.data.end(), foliageInstances.begin(), foliageInstances.end());

	return out;
}

}  // namespace InstancedVoxel
