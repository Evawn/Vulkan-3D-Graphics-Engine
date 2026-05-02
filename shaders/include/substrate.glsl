// World-grid occupancy substrate — GLSL query interface.
//
// Companion to src/rendering/voxel/Substrate.h. The includer `#include`s this
// file and gets one function — `traceShadowWorld` — that walks the substrate
// via DDA and returns 0 (occluded) or 1 (lit).
//
// Layered contributions (LIGHTING.md §3):
//   - **Foliage** (always): per-brick instance overlap CSR, ORd in via the
//     instance bitmask at brick entry.
//   - **Static / terrain** (opt-in via SUBSTRATE_TERRAIN): per-voxel check
//     into the bound terrain brickmap. Requires the includer to also define
//     a `terrain` SSBO and provide `ivec3 terrainOriginVoxel`. See
//     terrain_brickmap.glsl for the exact contract.
//
// REQUIRES the includer to bind these descriptors with these names:
//
//   layout(std430, ...) readonly buffer SubstrateBuffer { uint data[]; } substrate;
//   layout(std430, ...) readonly buffer InstanceBuffer  { InstanceData instances[]; } ib;
//   layout(std430, ...) readonly buffer BitmaskBuffer   { uint bits[]; } bitmask;
//
// If SUBSTRATE_TERRAIN is defined, additionally:
//
//   layout(std430, ...) readonly buffer TerrainBrickmapBuffer { uint data[]; } terrain;
//   ivec3 terrainOriginVoxel;   // world-voxel offset of terrain voxel (0,0,0)
//
// And these uniforms accessible by name:
//
//   meta.size       — ivec3, single-frame asset dimensions
//   meta.frameCount — int
//   frame.time      — float, animation clock
//
// `InstanceData` is the includer's struct; this file references its fields
// `position`, `animOffset`, and `yawIdx`.

#ifndef SUBSTRATE_GLSL_INCLUDED
#define SUBSTRATE_GLSL_INCLUDED

#ifdef SUBSTRATE_TERRAIN
#include "terrain_brickmap.glsl"
#endif

// Mirrors src/rendering-techniques/instanced-voxel/Substrate.h.
const int  kSubstrateBrickSize    = 8;
const uint kSubstrateEmptyBrick   = 0xFFFFFFFFu;
const uint kSubstrateHeaderWords  = 8u;

// ---- Header accessors --------------------------------------------------

struct SubstrateLayout {
	ivec3 gridDim;
	ivec3 originVoxel;
	uint  topGridBase;            // offset of top-level grid in `substrate.data[]`
	uint  foliageOffsetsBase;     // offset of foliage CSR offsets
	uint  foliageInstancesBase;   // offset of foliage CSR payload (instance ids)
};

SubstrateLayout substrateLayout() {
	SubstrateLayout L;
	L.gridDim     = ivec3(int(substrate.data[0]),
	                      int(substrate.data[1]),
	                      int(substrate.data[2]));
	// data[3] = brickSize (== kSubstrateBrickSize, sanity)
	L.originVoxel = ivec3(int(substrate.data[4]),
	                      int(substrate.data[5]),
	                      int(substrate.data[6]));
	uint cells    = uint(L.gridDim.x * L.gridDim.y * L.gridDim.z);
	uint brickCnt = substrate.data[7];
	L.topGridBase          = kSubstrateHeaderWords;
	L.foliageOffsetsBase   = L.topGridBase + cells;
	L.foliageInstancesBase = L.foliageOffsetsBase + brickCnt + 1u;
	return L;
}

// ---- Yaw inverse (cloud-cell → asset-cell) -----------------------------

// Inverse of the asset→cloud yaw mapping in Substrate.cpp::InstanceAabb.
// Yaw is one of {0°, 90°, 180°, 270°}; the inverse is integer-exact.
ivec3 substrateInverseYawCell(int yawIdx, ivec3 cloudCell) {
	switch (yawIdx & 0x3) {
		case 0:  return cloudCell;
		case 1:  return ivec3( cloudCell.y,    -cloudCell.x - 1, cloudCell.z);
		case 2:  return ivec3(-cloudCell.x - 1, -cloudCell.y - 1, cloudCell.z);
		default: return ivec3(-cloudCell.y - 1,  cloudCell.x,     cloudCell.z);
	}
}

// ---- Per-instance occupancy lookup -------------------------------------

int substrateInstanceFrame(float animOffset) {
	int fc = max(meta.frameCount, 1);
	int f  = int(floor(mod(frame.time + animOffset, float(fc))));
	if (f < 0) f += fc;
	return f;
}

// Bitmask layout (mirrors instanced_voxel_generate.comp):
//   bits[ frame * (numXWords * size.y * size.z)
//       + z     * (numXWords * size.y)
//       + y     *  numXWords
//       + xWord ]
bool substrateBitmaskTest(int frameIdx, ivec3 assetCell) {
	if (any(lessThan(assetCell, ivec3(0))) ||
	    any(greaterThanEqual(assetCell, meta.size))) return false;
	int numXWords = (meta.size.x + 31) / 32;
	int xWord = assetCell.x >> 5;
	int xBit  = assetCell.x & 31;
	int idx = frameIdx     * (numXWords * meta.size.y * meta.size.z)
	        + assetCell.z  * (numXWords * meta.size.y)
	        + assetCell.y  *  numXWords
	        + xWord;
	uint word = bitmask.bits[idx];
	return ((word >> uint(xBit)) & 1u) != 0u;
}

// "Does instance `instIdx` occupy `cloudVoxel` (in cloud-local voxel coords)
// *right now*?" Caller is responsible for converting world → cloud-local
// before calling — `traceShadowWorld` does this at its entry boundary.
bool substrateInstanceOccupies(uint instIdx, ivec3 cloudVoxel) {
	InstanceData inst = ib.instances[instIdx];
	ivec3 instPos    = ivec3(round(inst.position));
	ivec3 instLocal  = cloudVoxel - instPos;
	ivec3 assetCell  = substrateInverseYawCell(inst.yawIdx, instLocal);
	int   frameIdx   = substrateInstanceFrame(inst.animOffset);
	return substrateBitmaskTest(frameIdx, assetCell);
}

// ---- Shadow query -------------------------------------------------------

// World-space shadow trace.
//
// `originWorld`      — world-units ray origin.
// `dirWorld`         — unit direction (toward the sun).
// `maxDist`          — world-units cap.
// `worldVoxelSize`   — engine voxel pitch.
// `cloudOriginWorld` — the cloud node's world-space translation. The substrate
//                      stores positions in cloud-local voxel coords (substrate's
//                      `originVoxel` and `instances[i].position` are both
//                      cloud-local), so we fold world → cloud-local at the
//                      query boundary by subtracting this before the world →
//                      voxel divide. Cloud rotation must be identity and scale
//                      uniform = worldVoxelSize (LIGHTING.md §2 invariants).
// `skipCloudVoxel`   — receiver's own voxel in *cloud-local* voxel coords;
//                      exempt from occlusion. Pass any sentinel outside the
//                      substrate range to disable.
//
// Returns 1.0 (lit) if the ray reaches `maxDist` without occlusion, 0.0 on
// hit. The DDA is done in voxel space (cell size 1), with cached per-brick
// CSR state so every visited voxel that's in a populated brick costs one
// tight bounds check + a small instance loop.
float traceShadowWorld(vec3   originWorld,
                       vec3   dirWorld,
                       float  maxDist,
                       float  worldVoxelSize,
                       vec3   cloudOriginWorld,
                       ivec3  skipCloudVoxel)
{
	SubstrateLayout L = substrateLayout();
	uint cells = uint(L.gridDim.x * L.gridDim.y * L.gridDim.z);
	if (cells == 0u || substrate.data[7] == 0u) return 1.0;

	// Voxel-space ray, in cloud-local voxel coords (so substrate lookups —
	// which were built in cloud-local — work directly). Direction is scale-
	// invariant under uniform-scale cloud transforms; origin shifts by the
	// cloud's world translation before the worldVoxelSize divide.
	vec3  voxelOrigin = (originWorld - cloudOriginWorld) / worldVoxelSize;
	vec3  voxelDir    = dirWorld;
	float voxelMax    = maxDist     / worldVoxelSize;

	const float kEps = 1e-6;
	vec3 invDir = vec3(
		(abs(voxelDir.x) > kEps) ? 1.0 / voxelDir.x : 1e30,
		(abs(voxelDir.y) > kEps) ? 1.0 / voxelDir.y : 1e30,
		(abs(voxelDir.z) > kEps) ? 1.0 / voxelDir.z : 1e30);

	ivec3 stepSign = ivec3(
		voxelDir.x >= 0.0 ?  1 : -1,
		voxelDir.y >= 0.0 ?  1 : -1,
		voxelDir.z >= 0.0 ?  1 : -1);

	// Voxel cell containing the origin.
	ivec3 voxel = ivec3(floor(voxelOrigin));

	// Distance to next voxel boundary along each axis.
	vec3 vmin = vec3(voxel);
	vec3 vmax = vec3(voxel + 1);
	vec3 tMax = vec3(
		(voxelDir.x >= 0.0) ? (vmax.x - voxelOrigin.x) * invDir.x
		                    : (vmin.x - voxelOrigin.x) * invDir.x,
		(voxelDir.y >= 0.0) ? (vmax.y - voxelOrigin.y) * invDir.y
		                    : (vmin.y - voxelOrigin.y) * invDir.y,
		(voxelDir.z >= 0.0) ? (vmax.z - voxelOrigin.z) * invDir.z
		                    : (vmin.z - voxelOrigin.z) * invDir.z);
	vec3 tDelta = abs(invDir);

	// Cached brick state — brickCoord and CSR range get refreshed only when
	// the ray crosses a brick boundary, which is at most every 8 voxel steps.
	ivec3 cachedBrick = ivec3(0x7FFFFFFF);  // sentinel: forces first refresh
	uint  brickId     = kSubstrateEmptyBrick;
	uint  csrLo       = 0u;
	uint  csrHi       = 0u;

	float t = 0.0;

	// Iteration cap: rays longer than this are clipped to "lit" rather than
	// erroring. 2048 voxels = 52m at 0.0254/voxel (1 inch) — covers the
	// CombinedRenderer's 1024×1024-voxel island diagonal at any sun angle.
	const int kMaxSteps = 2048;

	for (int s = 0; s < kMaxSteps; ++s) {
		if (t > voxelMax) return 1.0;

		// Brick coordinates in the substrate grid (signed; negative below
		// the grid's anchor).
		ivec3 brickCoord;
		brickCoord.x = (voxel.x - L.originVoxel.x);
		brickCoord.y = (voxel.y - L.originVoxel.y);
		brickCoord.z = (voxel.z - L.originVoxel.z);
		// Floor-divide by 8 (signed-correct via arithmetic shift, since
		// kSubstrateBrickSize is a power of two and `>>` on int is implementation-
		// defined for negatives but on Vulkan-class GPUs is arithmetic shift).
		brickCoord.x = brickCoord.x >> 3;
		brickCoord.y = brickCoord.y >> 3;
		brickCoord.z = brickCoord.z >> 3;

		if (brickCoord != cachedBrick) {
			cachedBrick = brickCoord;
			bool inGrid = all(greaterThanEqual(brickCoord, ivec3(0))) &&
			              all(lessThan(brickCoord, L.gridDim));
			if (inGrid) {
				uint gridIdx = uint(brickCoord.x)
				             + uint(brickCoord.y) * uint(L.gridDim.x)
				             + uint(brickCoord.z) * uint(L.gridDim.x) * uint(L.gridDim.y);
				brickId = substrate.data[L.topGridBase + gridIdx];
				if (brickId != kSubstrateEmptyBrick) {
					csrLo = substrate.data[L.foliageOffsetsBase + brickId];
					csrHi = substrate.data[L.foliageOffsetsBase + brickId + 1u];
				}
			} else {
				brickId = kSubstrateEmptyBrick;
			}
		}

		if (brickId != kSubstrateEmptyBrick && voxel != skipCloudVoxel) {
			for (uint j = csrLo; j < csrHi; ++j) {
				uint instIdx = substrate.data[L.foliageInstancesBase + j];
				if (substrateInstanceOccupies(instIdx, voxel)) {
					return 0.0;
				}
			}
		}

#ifdef SUBSTRATE_TERRAIN
		// Static terrain contribution. Runs at every voxel step regardless
		// of the foliage substrate's grid extent — terrain may extend past
		// the foliage cloud's footprint, so the substrate grid bounds don't
		// gate it. Receiver's own voxel is exempt to avoid self-shadowing.
		if (voxel != skipCloudVoxel && terrainSolidAtWorldVoxel(voxel)) {
			return 0.0;
		}
#endif

		// Advance to the next voxel along whichever axis crosses first.
		if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
			t       = tMax.x;
			tMax.x += tDelta.x;
			voxel.x += stepSign.x;
		} else if (tMax.y <= tMax.z) {
			t       = tMax.y;
			tMax.y += tDelta.y;
			voxel.y += stepSign.y;
		} else {
			t       = tMax.z;
			tMax.z += tDelta.z;
			voxel.z += stepSign.z;
		}
	}

	// Iteration cap reached — treat as unoccluded so we don't leave a
	// black artifact on long rays.
	return 1.0;
}

#endif  // SUBSTRATE_GLSL_INCLUDED
