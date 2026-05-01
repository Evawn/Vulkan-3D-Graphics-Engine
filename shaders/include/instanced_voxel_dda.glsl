// DDA + voxel-sampling helpers shared by the InstancedVoxelTechnique's trace
// and shadow fragment shaders. Both shaders march through the same per-instance
// AABB the vertex shader rasterized, and both need to find the *real* voxel
// surface (not the cube AABB) — the shadow shader to write a tight depth, the
// trace shader to shade the hit.
//
// REQUIRES the includer to define:
//   layout(...) uniform usampler3D volume_sampler;
//   layout(...) uniform { ivec3 size; int frameCount; } meta;
//   flat int  vFrameIdx;                          // varying — slab index
//   vec3 g_voxel_local;                           // world-units-per-voxel (per axis)
//   vec3 g_local_origin;                          // pc.aabbMin
//   int  uMaxIterations;                          // iteration cap (e.g. frame.maxIterations)
//
// REQUIRES the includer to provide:
//   vec3 localToVoxel(vec3 p);
//   uint sampleMaterial(ivec3 voxelCoord);        // wraps texelFetch + frame slab
//   bool isSolidAt(ivec3 voxelCoord);             // sampleMaterial != 0u

struct DdaHit {
	bool  hit;
	ivec3 voxel;        // hit voxel index (in single-frame coords)
	bvec3 face;         // which axis was crossed last (the entry face)
	ivec3 step_sign;    // +1/-1 per axis along the march direction
	uint  matIdx;       // material index at the hit (0 = empty; never set when hit==false)
	float entryT;       // ray parameter at the entry face of the hit voxel.
	                    // The continuous hit point is `origin + direction * entryT`,
	                    // in the same frame the DDA was traced (instance-local).
	                    // Tracked because the *fractional* hit point (not the voxel
	                    // center) is what shadow rays need to produce diagonal
	                    // edges instead of face-quantized blocky shadows. 0.0 when
	                    // the very first sampled voxel was solid (camera-inside-blade).
	int   total_iters;  // for debug coloring
};

DdaHit traceLocal(vec3 origin, vec3 direction) {
	DdaHit h;
	h.hit = false;
	h.voxel = ivec3(0);
	h.face = bvec3(false);
	h.step_sign = ivec3(0);
	h.matIdx = 0u;
	h.entryT = 0.0;
	h.total_iters = 0;

	vec3 invDir = 1.0 / direction;

	// Origin is on the cube surface (the vertex shader's interpolated vLocalPos).
	// Nudge slightly inward so the first cell lookup doesn't sit on a boundary.
	vec3 p_voxel = localToVoxel(origin);
	p_voxel = clamp(p_voxel, vec3(0.001), vec3(meta.size) - 0.001);
	ivec3 voxel = clamp(ivec3(floor(p_voxel)), ivec3(0), meta.size - 1);

	ivec3 step_sign = ivec3(
		direction.x >= 0.0 ? 1 : -1,
		direction.y >= 0.0 ? 1 : -1,
		direction.z >= 0.0 ? 1 : -1
	);
	h.step_sign = step_sign;

	vec3 tDelta = abs(g_voxel_local / direction);

	// Per-axis t to reach the next voxel boundary along that axis.
	vec3 vmin = g_local_origin + vec3(voxel    ) * g_voxel_local;
	vec3 vmax = g_local_origin + vec3(voxel + 1) * g_voxel_local;
	vec3 tMaxAxis = vec3(
		direction.x >= 0.0 ? (vmax.x - origin.x) * invDir.x : (vmin.x - origin.x) * invDir.x,
		direction.y >= 0.0 ? (vmax.y - origin.y) * invDir.y : (vmin.y - origin.y) * invDir.y,
		direction.z >= 0.0 ? (vmax.z - origin.z) * invDir.z : (vmin.z - origin.z) * invDir.z
	);

	bvec3 last_face = bvec3(false);
	// `entry_t` = ray parameter at the entry face of the *current* voxel.
	// 0 on the first iteration (we started at `origin`, just inside the cell).
	// Each step records the t at the boundary we just crossed, *before*
	// advancing tMaxAxis to the next boundary.
	float entry_t = 0.0;

	for (int i = 0; i < uMaxIterations; i++) {
		if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, meta.size))) break;

		uint matIdx = sampleMaterial(voxel);
		if (matIdx != 0u) {
			h.hit = true;
			h.voxel = voxel;
			h.face = last_face;
			h.matIdx = matIdx;
			h.entryT = entry_t;
			h.total_iters = i;
			return h;
		}

		last_face = bvec3(false);
		if (tMaxAxis.x < tMaxAxis.y && tMaxAxis.x < tMaxAxis.z) {
			entry_t = tMaxAxis.x;
			tMaxAxis.x += tDelta.x;
			voxel.x += step_sign.x;
			last_face = bvec3(true, false, false);
		} else if (tMaxAxis.y < tMaxAxis.z) {
			entry_t = tMaxAxis.y;
			tMaxAxis.y += tDelta.y;
			voxel.y += step_sign.y;
			last_face = bvec3(false, true, false);
		} else {
			entry_t = tMaxAxis.z;
			tMaxAxis.z += tDelta.z;
			voxel.z += step_sign.z;
			last_face = bvec3(false, false, true);
		}
		h.total_iters = i + 1;
	}
	return h;
}
