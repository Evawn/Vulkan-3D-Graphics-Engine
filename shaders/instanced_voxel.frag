#version 450

// Instanced voxel mesh fragment — DDA inside the per-instance AABB.
//
// The vertex shader sets us up at a point on the cube surface in
// instance-local space (aabbMin..aabbMax). We march from there in the local
// equivalent of the world-space view direction, sampling the voxel image at
// `(x, y, z + frameIdx * size.z)` (frames as Z-slabs). On hit we read the
// palette and run the existing lighting + corner-AO helpers.

layout(location = 0) in vec3 vLocalPos;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) flat in int vFrameIdx;

layout(push_constant) uniform PushConstantBlock {
	mat4  cloudWorld;
	mat4  viewProj;
	vec3  cameraPos;
	int   maxIterations;
	vec3  skyColor;
	int   debugColor;
	vec3  sunDirection;
	float sunCosHalfAngle;
	vec3  sunColor;
	float sunIntensity;
	vec3  aabbMin;
	float ambientIntensity;
	vec3  aabbMax;
	float aoStrength;
	int   shadowsEnabled;
	float time;
	int   frameCount;
	int   _pad0;
} pc;

layout(set = 0, binding = 1) uniform usampler3D volume_sampler;
layout(set = 0, binding = 2) uniform sampler2D  palette_sampler;
layout(set = 0, binding = 3) uniform VolumeMeta {
	ivec3 size;          // single-frame dimensions
	int   frameCount;
} meta;

layout(location = 0) out vec4 outColor;

uint sampleMaterialAtFrame(ivec3 voxelCoord, int frame) {
	if (any(lessThan(voxelCoord, ivec3(0))) ||
	    any(greaterThanEqual(voxelCoord, meta.size))) return 0u;
	ivec3 c = ivec3(voxelCoord.x, voxelCoord.y, voxelCoord.z + frame * meta.size.z);
	return texelFetch(volume_sampler, c, 0).r;
}

uint sampleMaterial(ivec3 voxelCoord) {
	return sampleMaterialAtFrame(voxelCoord, vFrameIdx);
}

bool isSolidAt(ivec3 voxelCoord) {
	return sampleMaterial(voxelCoord) != 0u;
}

// Map instance-local AABB space ↔ voxel-grid space. The AABB spans aabbMin..aabbMax
// (instance-local), and the voxel grid is meta.size cells, so a voxel of edge
// length `voxel_local = (aabbMax - aabbMin) / meta.size` lives in local space.
// Declared before voxel_ao.glsl include because that helper calls worldToVoxel.
vec3  g_voxel_local;     // world-units-per-voxel (per axis)
vec3  g_local_origin;    // pc.aabbMin

vec3 localToVoxel(vec3 p) { return (p - g_local_origin) / g_voxel_local; }
// voxel_ao.glsl expects a `worldToVoxel(vec3)` — for our per-instance
// technique, "world" is the AABB-local space that we're DDA'ing in.
vec3 worldToVoxel(vec3 p) { return localToVoxel(p); }

#include "voxel_ao.glsl"

struct Hit {
	bool  hit;
	float t;
	ivec3 voxel;
	bvec3 face;
	ivec3 step_sign;
	uint  matIdx;
	int   total_iters;
};

Hit traceLocal(vec3 origin, vec3 direction) {
	Hit h;
	h.hit = false; h.t = 0.0; h.voxel = ivec3(0);
	h.face = bvec3(false); h.step_sign = ivec3(0); h.matIdx = 0u; h.total_iters = 0;

	// Origin already inside AABB (we entered at the cube fragment); start from
	// vLocalPos slightly nudged forward to avoid floating-point edge cases.
	vec3 invDir = 1.0 / direction;

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

	for (int i = 0; i < pc.maxIterations; i++) {
		if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, meta.size))) break;

		uint matIdx = sampleMaterial(voxel);
		if (matIdx != 0u) {
			h.hit = true;
			h.voxel = voxel;
			h.face = last_face;
			h.matIdx = matIdx;
			h.total_iters = i;
			return h;
		}

		last_face = bvec3(false);
		if (tMaxAxis.x < tMaxAxis.y && tMaxAxis.x < tMaxAxis.z) {
			tMaxAxis.x += tDelta.x;
			voxel.x += step_sign.x;
			last_face = bvec3(true, false, false);
		} else if (tMaxAxis.y < tMaxAxis.z) {
			tMaxAxis.y += tDelta.y;
			voxel.y += step_sign.y;
			last_face = bvec3(false, true, false);
		} else {
			tMaxAxis.z += tDelta.z;
			voxel.z += step_sign.z;
			last_face = bvec3(false, false, true);
		}
		h.total_iters = i + 1;
	}
	return h;
}

void main() {
	g_local_origin = pc.aabbMin;
	g_voxel_local  = (pc.aabbMax - pc.aabbMin) / vec3(meta.size);

	// Compute the ray *in instance-local space* by transforming the world ray
	// through the inverse of (cloudWorld * per-instance T*R*S). For v1 we
	// approximate by treating the local-space ray direction as the
	// world-direction's projection onto the cube's local frame: the per-cloud
	// transform is approximately rigid (scale=1, rotation present), so the
	// ray direction in local space = inverse(cloudWorld) * worldDir. This
	// loses fidelity if the per-instance scale is non-uniform — fine for v1
	// foliage where instances are uniformly scaled.
	mat3 worldToLocal = transpose(mat3(pc.cloudWorld));
	vec3 worldDir = normalize(vWorldPos - pc.cameraPos);
	vec3 localDir = normalize(worldToLocal * worldDir);

	Hit h = traceLocal(vLocalPos + localDir * 0.0001, localDir);

	if (!h.hit) {
		// Discard unhit cube interior — sky shows through. Foliage is sparse.
		discard;
	}

	vec4 albedo = texelFetch(palette_sampler, ivec2(h.matIdx, 0), 0);
	vec3 normal = -vec3(h.step_sign) * vec3(h.face);

	float ao = 1.0;
	if (pc.aoStrength > 0.0) {
		// We need a hitPos for cornerAO; use the voxel's center as a v1 stand-in.
		vec3 hitLocal = g_local_origin + (vec3(h.voxel) + 0.5) * g_voxel_local;
		float raw = cornerAO(h.voxel, h.face, h.step_sign, hitLocal);
		ao = mix(1.0, raw, pc.aoStrength);
	}

	// Sun direction is world-space; transform to local for NdotL.
	vec3 localSunDir = normalize(worldToLocal * pc.sunDirection);
	float NdotL = max(0.0, dot(normal, localSunDir));

	vec3 ambient = pc.skyColor * pc.ambientIntensity * ao;
	vec3 direct  = pc.sunColor * pc.sunIntensity * NdotL;
	vec3 lit = albedo.rgb * (ambient + direct);

	if (pc.debugColor != 0) {
		lit -= vec3(float(h.total_iters) / 250.0);
	}

	outColor = vec4(lit, 1.0);
}
