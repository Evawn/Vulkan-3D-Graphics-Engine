#include "ShadowBrickmap.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>

namespace ShadowBrickmap {

namespace {

constexpr int kBrick = static_cast<int>(BrickGrid::kBrickSize);

// Per-instance maximal voxel AABB in world coords, half-open [lo, hi).
// "Maximal" = the union over all animation frames. For v1 grass the
// per-frame painted bounds vary slightly under sway; the conservative
// bound is the asset's full [0, size) box, yaw-rotated and translated.
// Topology is sized off this; per-frame compute writes are sparser.
struct VoxelAabb { glm::ivec3 lo, hi; };

VoxelAabb InstanceMaxAabb(const InstanceInput& inst, glm::uvec3 assetSize) {
	const int sx = static_cast<int>(assetSize.x);
	const int sy = static_cast<int>(assetSize.y);
	const int sz = static_cast<int>(assetSize.z);
	glm::ivec3 lo, hi;
	switch (inst.yawIdx & 0x3) {
		case 0: lo = {  0,    0,   0}; hi = { sx,  sy,  sz}; break;
		case 1: lo = {-sy,    0,   0}; hi = {  0,  sx,  sz}; break;
		case 2: lo = {-sx,  -sy,   0}; hi = {  0,   0,  sz}; break;
		default: lo = {  0,  -sx,   0}; hi = { sy,   0,  sz}; break;
	}
	return { lo + inst.worldVoxelPos, hi + inst.worldVoxelPos };
}

// CPU-side terrain probe. Walks the BrickmapData buffer the same way the
// GPU shader does. `terrainLocal` is in terrain-local voxels (caller
// subtracts terrain.originVoxel).
bool TerrainSolid(const BrickmapData& bm, glm::ivec3 v) {
	if (v.x < 0 || v.y < 0 || v.z < 0) return false;
	if (v.x >= int(bm.volumeSize.x) ||
	    v.y >= int(bm.volumeSize.y) ||
	    v.z >= int(bm.volumeSize.z)) return false;

	const int bs = static_cast<int>(bm.brickSize);
	const glm::ivec3 brickCell(v.x / bs, v.y / bs, v.z / bs);
	const glm::ivec3 local(v.x - brickCell.x * bs,
	                       v.y - brickCell.y * bs,
	                       v.z - brickCell.z * bs);
	const uint32_t gridIdx = BrickGrid::TopGridIndex(brickCell, bm.gridDim);
	const uint32_t brickIndex = bm.data[BrickGrid::kHeaderWords + gridIdx];
	if (brickIndex == BrickGrid::kEmptyBrick) return false;

	const uint32_t topCells = bm.gridDim.x * bm.gridDim.y * bm.gridDim.z;
	const uint32_t linear   = BrickGrid::BrickVoxelLinear(local);
	const uint32_t wordIdx  = linear >> 2;
	const uint32_t laneBit  = (linear & 3u) * 8u;
	const uint32_t word = bm.data[BrickGrid::kHeaderWords + topCells
	                              + brickIndex * BrickGrid::kPaletteWordsPerBrick
	                              + wordIdx];
	const uint32_t mat = (word >> laneBit) & 0xFFu;
	return mat != 0u;
}

inline void SetBitmaskBit(uint32_t* poolBase, uint32_t brickId, glm::ivec3 local) {
	const uint32_t bitLinear = BrickGrid::BrickVoxelLinear(local);
	const uint32_t wordIdx   = bitLinear >> 5;
	const uint32_t bitIdx    = bitLinear & 31u;
	poolBase[brickId * BrickGrid::kBitmaskWordsPerBrick + wordIdx] |= (1u << bitIdx);
}

}  // namespace

uint32_t UpperBoundWords(glm::uvec3 terrainVolumeSize,
                         glm::uvec3 assetSize,
                         uint32_t   instanceCount)
{
	// Foliage AABB overhang past the terrain's footprint, in world voxels.
	const int yawPad   = static_cast<int>(std::max(assetSize.x, assetSize.y));
	const int spanX    = static_cast<int>(terrainVolumeSize.x) + (instanceCount > 0 ? 2 * yawPad : 0);
	const int spanY    = static_cast<int>(terrainVolumeSize.y) + (instanceCount > 0 ? 2 * yawPad : 0);
	const int spanZ    = static_cast<int>(terrainVolumeSize.z) +
	                     (instanceCount > 0 ? static_cast<int>(assetSize.z) : 0);

	const uint32_t gx = static_cast<uint32_t>(BrickGrid::CeilDivBrick(spanX));
	const uint32_t gy = static_cast<uint32_t>(BrickGrid::CeilDivBrick(spanY));
	const uint32_t gz = static_cast<uint32_t>(BrickGrid::CeilDivBrick(spanZ));
	const uint64_t topGridCells = uint64_t(gx) * gy * gz;
	const uint64_t maxBricks    = topGridCells;

	const uint64_t total = uint64_t(BrickGrid::kHeaderWords)
	                     + topGridCells
	                     + maxBricks * BrickGrid::kBitmaskWordsPerBrick   // static pool
	                     + maxBricks * BrickGrid::kBitmaskWordsPerBrick;  // dynamic pool
	// std::min wrapped because the worst case can blow past uint32 on huge
	// terrains; callers clamp to a hardware-feasible buffer size.
	return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
}

uint32_t UpperBoundInstanceBricks(uint32_t   instanceCount,
                                  glm::uvec3 assetSize)
{
	const auto axisSpanBricks = [](uint32_t s) {
		return (s + BrickGrid::kBrickSize - 1u) / BrickGrid::kBrickSize + 1u;
	};
	const uint32_t bricksPerInst = axisSpanBricks(assetSize.x) *
	                                axisSpanBricks(assetSize.y) *
	                                axisSpanBricks(assetSize.z);
	return instanceCount * bricksPerInst;
}

Build BuildShadowBrickmap(const BrickmapData&         terrain,
                          const InstanceInput*        instances,
                          uint32_t                    instanceCount,
                          glm::uvec3                  assetSize)
{
	Build out;

	// 1. Compute the union AABB in world voxels.
	//
	// Terrain occupies [terrain.originVoxel, terrain.originVoxel + volumeSize)
	// in world voxels. Each instance contributes its maximal AABB. The shadow
	// brickmap's anchor (originVoxel) is brick-aligned to the lo corner of
	// the union; the gridDim is sized to cover the hi corner.
	glm::ivec3 worldLo, worldHi;
	const bool hasTerrain   = (terrain.volumeSize.x > 0 &&
	                            terrain.volumeSize.y > 0 &&
	                            terrain.volumeSize.z > 0);
	const bool hasFoliage   = (instanceCount > 0);

	if (!hasTerrain && !hasFoliage) {
		out.header.brickCount  = 0;
		out.header.gridDim     = glm::uvec3(0);
		out.header.originVoxel = glm::ivec3(0);
		out.data.assign(BrickGrid::kHeaderWords, 0u);
		out.data[3] = BrickGrid::kBrickSize;
		out.topGridBase = BrickGrid::kHeaderWords;
		out.staticPoolBase = BrickGrid::kHeaderWords;
		out.dynamicPoolBase = BrickGrid::kHeaderWords;
		return out;
	}

	if (hasTerrain) {
		worldLo = terrain.originVoxel;
		worldHi = terrain.originVoxel + glm::ivec3(terrain.volumeSize);
	} else {
		worldLo = glm::ivec3( INT_MAX);
		worldHi = glm::ivec3(-INT_MAX);
	}
	for (uint32_t i = 0; i < instanceCount; ++i) {
		const auto a = InstanceMaxAabb(instances[i], assetSize);
		worldLo = glm::min(worldLo, a.lo);
		worldHi = glm::max(worldHi, a.hi);
	}

	// 2. Brick-align: origin = floor(worldLo / 8) * 8, gridDim = ceil((hi - origin) / 8).
	const glm::ivec3 originVoxel(
		BrickGrid::FloorDivBrick(worldLo.x) * kBrick,
		BrickGrid::FloorDivBrick(worldLo.y) * kBrick,
		BrickGrid::FloorDivBrick(worldLo.z) * kBrick);
	const glm::ivec3 maxVoxel(
		BrickGrid::CeilDivBrick(worldHi.x) * kBrick,
		BrickGrid::CeilDivBrick(worldHi.y) * kBrick,
		BrickGrid::CeilDivBrick(worldHi.z) * kBrick);
	const glm::uvec3 gridDim(
		static_cast<uint32_t>((maxVoxel.x - originVoxel.x) / kBrick),
		static_cast<uint32_t>((maxVoxel.y - originVoxel.y) / kBrick),
		static_cast<uint32_t>((maxVoxel.z - originVoxel.z) / kBrick));
	const uint32_t topGridCells = gridDim.x * gridDim.y * gridDim.z;

	// 3. Collect the set of bricks that need to exist.
	//
	// Bricks are added if EITHER (a) any terrain voxel lying inside the brick
	// is solid OR (b) any instance's maximal AABB intersects the brick. We
	// allocate a brick id only on first hit; the order is brick-coord-major
	// (x fastest, then y, then z) so we touch the static layer densely.
	std::vector<uint32_t> topGrid(topGridCells, BrickGrid::kEmptyBrick);

	auto allocBrick = [&](glm::ivec3 brickCoord, uint32_t& brickCount) -> uint32_t {
		const uint32_t gridIdx = BrickGrid::TopGridIndex(brickCoord, gridDim);
		if (topGrid[gridIdx] == BrickGrid::kEmptyBrick) {
			topGrid[gridIdx] = brickCount++;
		}
		return topGrid[gridIdx];
	};

	uint32_t brickCount = 0;

	// (a) Terrain bricks. The terrain brickmap already encodes "this brick
	// has any solid voxel" via its top-grid sentinel — walk that, project
	// each populated terrain brick into shadow-brickmap brick coords, and
	// allocate a shadow brick at the same world position. We don't need to
	// re-test individual voxels here because the *bit* writes happen below;
	// we only need brick allocation at this step.
	if (hasTerrain) {
		for (uint32_t bz = 0; bz < terrain.gridDim.z; ++bz)
		for (uint32_t by = 0; by < terrain.gridDim.y; ++by)
		for (uint32_t bx = 0; bx < terrain.gridDim.x; ++bx) {
			const uint32_t gridIdx = bx
			                       + by * terrain.gridDim.x
			                       + bz * terrain.gridDim.x * terrain.gridDim.y;
			const uint32_t brickIdx = terrain.data[BrickGrid::kHeaderWords + gridIdx];
			if (brickIdx == BrickGrid::kEmptyBrick) continue;

			const glm::ivec3 worldBrickVoxel = terrain.originVoxel +
				glm::ivec3(int(bx), int(by), int(bz)) * kBrick;
			const glm::ivec3 shadowBrick(
				(worldBrickVoxel.x - originVoxel.x) / kBrick,
				(worldBrickVoxel.y - originVoxel.y) / kBrick,
				(worldBrickVoxel.z - originVoxel.z) / kBrick);
			(void)allocBrick(shadowBrick, brickCount);
		}
	}

	// (b) Foliage bricks: for each instance, walk its maximal-AABB brick
	// extent and allocate a shadow brick at every covered cell.
	for (uint32_t i = 0; i < instanceCount; ++i) {
		const auto a = InstanceMaxAabb(instances[i], assetSize);
		const glm::ivec3 brickLo(
			(a.lo.x     - originVoxel.x) / kBrick,
			(a.lo.y     - originVoxel.y) / kBrick,
			(a.lo.z     - originVoxel.z) / kBrick);
		const glm::ivec3 brickHi(
			(a.hi.x - 1 - originVoxel.x) / kBrick,
			(a.hi.y - 1 - originVoxel.y) / kBrick,
			(a.hi.z - 1 - originVoxel.z) / kBrick);
		for (int bz = brickLo.z; bz <= brickHi.z; ++bz)
		for (int by = brickLo.y; by <= brickHi.y; ++by)
		for (int bx = brickLo.x; bx <= brickHi.x; ++bx) {
			assert(bx >= 0 && by >= 0 && bz >= 0);
			assert(bx < int(gridDim.x) &&
			       by < int(gridDim.y) &&
			       bz < int(gridDim.z));
			(void)allocBrick(glm::ivec3(bx, by, bz), brickCount);
		}
	}

	// 4. Allocate the buffer: header + top-grid + 2 × (brickCount × 16 words).
	const uint64_t poolWords = uint64_t(brickCount) * BrickGrid::kBitmaskWordsPerBrick;
	const uint64_t totalWords = uint64_t(BrickGrid::kHeaderWords)
	                          + uint64_t(topGridCells)
	                          + poolWords + poolWords;
	out.data.assign(totalWords, 0u);

	out.topGridBase     = BrickGrid::kHeaderWords;
	out.staticPoolBase  = out.topGridBase + topGridCells;
	out.dynamicPoolBase = out.staticPoolBase + static_cast<uint32_t>(poolWords);

	// 5. Write header.
	out.data[0] = gridDim.x;
	out.data[1] = gridDim.y;
	out.data[2] = gridDim.z;
	out.data[3] = BrickGrid::kBrickSize;
	out.data[4] = static_cast<uint32_t>(originVoxel.x);
	out.data[5] = static_cast<uint32_t>(originVoxel.y);
	out.data[6] = static_cast<uint32_t>(originVoxel.z);
	out.data[7] = brickCount;

	// 6. Top-level grid.
	std::memcpy(out.data.data() + out.topGridBase,
	            topGrid.data(),
	            topGridCells * sizeof(uint32_t));

	out.header.volumeSize  = glm::uvec3(static_cast<uint32_t>(maxVoxel.x - originVoxel.x),
	                                    static_cast<uint32_t>(maxVoxel.y - originVoxel.y),
	                                    static_cast<uint32_t>(maxVoxel.z - originVoxel.z));
	out.header.gridDim     = gridDim;
	out.header.originVoxel = originVoxel;
	out.header.brickCount  = brickCount;

	// 7. Static pool: walk every solid terrain voxel and OR its bit into the
	// owning shadow brick. Use the per-cell brickIdx from `topGrid`.
	if (hasTerrain) {
		uint32_t* staticPool = out.data.data() + out.staticPoolBase;
		for (int wz = 0; wz < int(terrain.volumeSize.z); ++wz)
		for (int wy = 0; wy < int(terrain.volumeSize.y); ++wy)
		for (int wx = 0; wx < int(terrain.volumeSize.x); ++wx) {
			if (!TerrainSolid(terrain, glm::ivec3(wx, wy, wz))) continue;

			const glm::ivec3 worldVx = terrain.originVoxel + glm::ivec3(wx, wy, wz);
			const glm::ivec3 shadowVx(worldVx.x - originVoxel.x,
			                          worldVx.y - originVoxel.y,
			                          worldVx.z - originVoxel.z);
			const glm::ivec3 brickCoord(shadowVx.x / kBrick,
			                            shadowVx.y / kBrick,
			                            shadowVx.z / kBrick);
			const glm::ivec3 local(shadowVx.x - brickCoord.x * kBrick,
			                       shadowVx.y - brickCoord.y * kBrick,
			                       shadowVx.z - brickCoord.z * kBrick);
			const uint32_t brickId = topGrid[BrickGrid::TopGridIndex(brickCoord, gridDim)];
			assert(brickId != BrickGrid::kEmptyBrick);
			SetBitmaskBit(staticPool, brickId, local);
		}
	}

	// 8. Build instanceBricks for the per-frame foliage compute. One entry
	// per (instance, world-brick) pair — each entry tells the compute pass
	// "this thread block writes brickId on behalf of this instance". The
	// compute pass dispatches over (instanceBricks.size() × brickVoxelCount).
	out.instanceBricks.reserve(static_cast<size_t>(instanceCount) * 4);
	for (uint32_t i = 0; i < instanceCount; ++i) {
		const auto a = InstanceMaxAabb(instances[i], assetSize);
		const glm::ivec3 brickLo(
			(a.lo.x     - originVoxel.x) / kBrick,
			(a.lo.y     - originVoxel.y) / kBrick,
			(a.lo.z     - originVoxel.z) / kBrick);
		const glm::ivec3 brickHi(
			(a.hi.x - 1 - originVoxel.x) / kBrick,
			(a.hi.y - 1 - originVoxel.y) / kBrick,
			(a.hi.z - 1 - originVoxel.z) / kBrick);
		for (int bz = brickLo.z; bz <= brickHi.z; ++bz)
		for (int by = brickLo.y; by <= brickHi.y; ++by)
		for (int bx = brickLo.x; bx <= brickHi.x; ++bx) {
			const uint32_t brickId = topGrid[BrickGrid::TopGridIndex(glm::ivec3(bx, by, bz), gridDim)];
			assert(brickId != BrickGrid::kEmptyBrick);
			InstanceBrick ib{};
			ib.instanceIdx        = i;
			ib.brickId            = brickId;
			ib.brickWorldVoxelX   = originVoxel.x + bx * kBrick;
			ib.brickWorldVoxelY   = originVoxel.y + by * kBrick;
			ib.brickWorldVoxelZ   = originVoxel.z + bz * kBrick;
			out.instanceBricks.push_back(ib);
		}
	}

	return out;
}

}  // namespace ShadowBrickmap
