// Shadow trace — two-level DDA over the shadow occupancy brickmap.
//
// Companion to src/rendering/voxel/ShadowBrickmap.h. The includer #includes
// this file and gets one function — `traceShadowWorld` — that walks the
// shadow brickmap with an outer brick DDA + inner per-voxel DDA. Empty
// bricks cost ONE outer step (= 8 voxels of distance), populated bricks
// descend into a per-voxel inner DDA (≤ 24 inner steps for a single brick
// crossing). See docs/SHADOW-BRICKS.md §6.
//
// REQUIRES the includer to bind ONE SSBO with the symbol `shadowBM`:
//
//   layout(std430, ...) readonly buffer ShadowBrickmapBuffer { uint data[]; } shadowBM;
//
// And to provide ONE uniform value accessible by name:
//
//   int frame_maxShadowBrickSteps;   // outer-DDA cap; typically 256
//
// (The trace shaders read this out of FrameUbo into a local `int
// frame_maxShadowBrickSteps` before the include — see combined_*_trace.frag.)

#ifndef SHADOW_TRACE_GLSL_INCLUDED
#define SHADOW_TRACE_GLSL_INCLUDED

#include "shadow_brickmap.glsl"

// World-space shadow trace.
//
// `originWorld`     — ray origin in world units.
// `dirWorld`        — unit ray direction (toward the sun).
// `maxDist`         — world-units distance cap. Soft — the brick-step cap
//                     usually fires first.
// `worldVoxelSize`  — engine voxel pitch (m/voxel).
// `skipWorldVoxel`  — receiver's own voxel in WORLD voxel coords; exempt
//                     from occlusion to avoid self-shadowing at the hit
//                     face. Pass any sentinel outside the brickmap range
//                     to disable.
//
// Returns 1.0 (lit) on cap or AABB exit, 0.0 (occluded) on hit.
float traceShadowWorld(vec3   originWorld,
                       vec3   dirWorld,
                       float  maxDist,
                       float  worldVoxelSize,
                       ivec3  skipWorldVoxel)
{
	ShadowLayout L = shadowBrickmapLayout();
	if (L.brickCount == 0u) return 1.0;

	// World → shadow-brickmap-local voxel coords (units = world voxels,
	// origin at the brickmap's anchor).
	vec3  voxelOrigin = (originWorld / worldVoxelSize) - vec3(L.originVoxel);
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

	// ---- Outer DDA: stride 8 voxels along whichever axis crosses first.
	//
	// brickCoord is the brick index in shadow-brickmap-local space (>=0 by
	// construction once the ray enters the grid; can be negative before
	// entry). brickEntry/Exit are voxel-coord boundaries in the same frame.
	ivec3 brickCoord = ivec3(floor(voxelOrigin)) >> int(kBrickSizeShift);

	// tMax along each axis: distance (in voxel units) to the next brick
	// boundary. tDelta: distance between consecutive brick boundaries on
	// the same axis = 8 voxels / |dir component|.
	vec3 brickEntry = vec3(brickCoord * int(kBrickSize));
	vec3 brickExit  = brickEntry + vec3(int(kBrickSize));
	vec3 tMaxOuter = vec3(
		(voxelDir.x >= 0.0) ? (brickExit.x - voxelOrigin.x) * invDir.x
		                    : (brickEntry.x - voxelOrigin.x) * invDir.x,
		(voxelDir.y >= 0.0) ? (brickExit.y - voxelOrigin.y) * invDir.y
		                    : (brickEntry.y - voxelOrigin.y) * invDir.y,
		(voxelDir.z >= 0.0) ? (brickExit.z - voxelOrigin.z) * invDir.z
		                    : (brickEntry.z - voxelOrigin.z) * invDir.z);
	vec3 tDeltaOuter = abs(invDir) * float(int(kBrickSize));

	// Inner-DDA tDelta is the per-voxel version of the above.
	vec3 tDeltaInner = abs(invDir);

	float tOuterStart = 0.0;

	for (int o = 0; o < frame_maxShadowBrickSteps; ++o) {
		if (tOuterStart > voxelMax) return 1.0;

		uint brickId = shadowBrickAtCell(brickCoord, L);
		if (brickId != kEmptyBrick) {
			// ---- Inner per-voxel DDA inside this brick only.
			//
			// Walk at most 3 × kBrickSize = 24 voxels — the worst-case voxel
			// count a ray can traverse in a single 8³ brick (corner-to-
			// corner with axis-tied steps).
			//
			// Initialize the inner ray at the *current* outer-t entry into
			// this brick. Exit if the voxel leaves the brick along the
			// stepSign axis on which it left.
			vec3 innerOrigin = voxelOrigin + voxelDir * tOuterStart;
			ivec3 voxel      = ivec3(floor(innerOrigin));
			ivec3 brickLocal = voxel - brickCoord * int(kBrickSize);
			// Numerical safety: the floor may push us one voxel off when
			// innerOrigin sits exactly on a brick face. Clamp local to
			// [0, 8) and re-derive voxel.
			brickLocal = clamp(brickLocal, ivec3(0), ivec3(int(kBrickSize) - 1));
			voxel      = brickCoord * int(kBrickSize) + brickLocal;

			vec3 vmin = vec3(voxel);
			vec3 vmax = vec3(voxel + 1);
			vec3 tMaxInner = vec3(
				(voxelDir.x >= 0.0) ? (vmax.x - voxelOrigin.x) * invDir.x
				                    : (vmin.x - voxelOrigin.x) * invDir.x,
				(voxelDir.y >= 0.0) ? (vmax.y - voxelOrigin.y) * invDir.y
				                    : (vmin.y - voxelOrigin.y) * invDir.y,
				(voxelDir.z >= 0.0) ? (vmax.z - voxelOrigin.z) * invDir.z
				                    : (vmin.z - voxelOrigin.z) * invDir.z);

			const int kInnerCap = 24;   // 3 × kBrickSize — geometric worst case
			for (int i = 0; i < kInnerCap; ++i) {
				// Voxel solid? Receiver's own voxel is exempt — the skip
				// test sits on the inner loop because it's a per-voxel
				// concern, not a per-brick one.
				ivec3 worldVoxel = voxel + L.originVoxel;
				if (worldVoxel != skipWorldVoxel) {
					if (shadowVoxelSolid(brickId, brickLocal, L)) return 0.0;
				}

				// Step inner DDA along whichever axis crosses first.
				if (tMaxInner.x <= tMaxInner.y && tMaxInner.x <= tMaxInner.z) {
					tMaxInner.x += tDeltaInner.x;
					voxel.x     += stepSign.x;
					brickLocal.x += stepSign.x;
				} else if (tMaxInner.y <= tMaxInner.z) {
					tMaxInner.y += tDeltaInner.y;
					voxel.y     += stepSign.y;
					brickLocal.y += stepSign.y;
				} else {
					tMaxInner.z += tDeltaInner.z;
					voxel.z     += stepSign.z;
					brickLocal.z += stepSign.z;
				}

				// Brick exit?
				if (any(lessThan(brickLocal, ivec3(0))) ||
				    any(greaterThanEqual(brickLocal, ivec3(int(kBrickSize))))) break;
			}
		}

		// Advance outer DDA to the next brick along whichever axis
		// crosses first.
		if (tMaxOuter.x <= tMaxOuter.y && tMaxOuter.x <= tMaxOuter.z) {
			tOuterStart   = tMaxOuter.x;
			tMaxOuter.x  += tDeltaOuter.x;
			brickCoord.x += stepSign.x;
		} else if (tMaxOuter.y <= tMaxOuter.z) {
			tOuterStart   = tMaxOuter.y;
			tMaxOuter.y  += tDeltaOuter.y;
			brickCoord.y += stepSign.y;
		} else {
			tOuterStart   = tMaxOuter.z;
			tMaxOuter.z  += tDeltaOuter.z;
			brickCoord.z += stepSign.z;
		}
	}

	// Outer-step cap reached → treat as lit. Same failure mode as the
	// original substrate query: long rays don't smear into black.
	return 1.0;
}

#endif  // SHADOW_TRACE_GLSL_INCLUDED
