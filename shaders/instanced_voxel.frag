#version 450

// Instanced voxel mesh fragment — DDA inside the per-instance AABB.
//
// The vertex shader sets us up at a point on the cube surface in
// instance-local space (aabbMin..aabbMax). We march from there in the local
// equivalent of the world-space view direction, sampling the voxel image at
// `(x, y, z + frameIdx * size.z)` (frames as Z-slabs). On hit we read the
// palette and run the existing lighting + corner-AO helpers.
//
// Direct lighting is computed by walking the world-grid occupancy substrate
// (substrate.glsl / Substrate.h) from the world-space hit point toward the
// sun. Voxel-perfect inter-instance occlusion: any blade casts on any other
// blade through the same DDA query — no shadow-map resolution loss, no
// cube-shaped silhouettes.

layout(location = 0) in vec3 vLocalPos;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) flat in int   vFrameIdx;
layout(location = 3) flat in vec4  vInstRot;     // per-instance quaternion
layout(location = 4) flat in vec3  vInstPos;     // per-instance position (cloud-local voxels)
layout(location = 5) flat in float vInstScale;   // per-instance uniform scale (post-A: always 1)

layout(push_constant) uniform DrawPushConstantBlock {
	mat4 cloudWorld;
	vec3 aabbMin; float _pad0;
	vec3 aabbMax; float _pad1;
} pc;

// Per-instance state for the substrate's secondary lookups (instances OTHER
// than the one this fragment belongs to). The vertex stage already binds
// this for transform; we re-declare it here so the frag can index it.
struct InstanceData {
	vec3  position;       float scale;
	vec4  rotation;
	float animOffset;
	float _pad0;
	int   yawIdx;
	float _pad2;
};
layout(std430, set = 0, binding = 0) readonly buffer InstanceBuffer {
	InstanceData instances[];
} ib;

layout(set = 0, binding = 1) uniform usampler3D volume_sampler;
layout(set = 0, binding = 2) uniform sampler2D  palette_sampler;
layout(set = 0, binding = 3) uniform VolumeMeta {
	ivec3 size;          // single-frame dimensions
	int   frameCount;
} meta;

// Per-frame state (camera/sun/sky/time/iteration). Shared with the sky pass.
// Layout must mirror InstancedVoxelFrameUbo in InstancedVoxelTechnique.cpp
// exactly — std140 padding rules pack vec3+scalar pairs into single 16 B slots.
layout(set = 0, binding = 4) uniform FrameUbo {
	mat4  viewProj;
	mat4  ndcToWorld;
	vec3  cameraPos;         int   maxIterations;
	vec3  skyColor;          int   debugColor;
	vec3  sunDirection;      float sunCosHalfAngle;
	vec3  sunColor;          float sunIntensity;
	float ambientIntensity;
	float aoStrength;
	int   shadowsEnabled;
	float time;
	int   frameCount;
	float shadowBiasConstant;
	float shadowBiasSlope;
	float worldVoxelSize;
} frame;

// World-grid substrate. Walked by `traceShadowWorld` (substrate.glsl).
layout(std430, set = 0, binding = 6) readonly buffer SubstrateBuffer {
	uint data[];
} substrate;

// Per-asset, per-frame occupancy bitmask. Read by substrate's instance test.
layout(std430, set = 0, binding = 7) readonly buffer BitmaskBuffer {
	uint bits[];
} bitmask;

layout(location = 0) out vec4 outColor;

// Conjugate of a unit quaternion is its inverse. Cheaper than inverse().
vec4 quatConjugate(vec4 q) { return vec4(-q.xyz, q.w); }
vec3 quatRotate(vec4 q, vec3 v) {
	vec3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

uint sampleMaterialAtFrame(ivec3 voxelCoord, int f) {
	if (any(lessThan(voxelCoord, ivec3(0))) ||
	    any(greaterThanEqual(voxelCoord, meta.size))) return 0u;
	ivec3 c = ivec3(voxelCoord.x, voxelCoord.y, voxelCoord.z + f * meta.size.z);
	return texelFetch(volume_sampler, c, 0).r;
}

uint sampleMaterial(ivec3 voxelCoord) {
	return sampleMaterialAtFrame(voxelCoord, vFrameIdx);
}

bool isSolidAt(ivec3 voxelCoord) {
	return sampleMaterial(voxelCoord) != 0u;
}

// Map instance-local AABB space ↔ voxel-grid space.
vec3  g_voxel_local;
vec3  g_local_origin;
int   uMaxIterations;     // pulled from frame UBO at main() entry — DDA helper reads this

vec3 localToVoxel(vec3 p) { return (p - g_local_origin) / g_voxel_local; }
vec3 worldToVoxel(vec3 p) { return localToVoxel(p); }

#include "voxel_ao.glsl"
#include "instanced_voxel_dda.glsl"
#include "substrate.glsl"

// Convert an instance-local position into world space using the same chain the
// vertex shader applied: `cloudWorld * (R_inst * (S_inst * p) + T_inst)`.
vec3 instanceLocalToWorld(vec3 pLocal) {
	vec3 inCloud = quatRotate(vInstRot, pLocal * vInstScale) + vInstPos;
	return (pc.cloudWorld * vec4(inCloud, 1.0)).xyz;
}

void main() {
	g_local_origin = pc.aabbMin;
	g_voxel_local  = (pc.aabbMax - pc.aabbMin) / vec3(meta.size);
	uMaxIterations = frame.maxIterations;

	// World ray → instance-AABB-local ray.
	mat3 cloudWorldInv = transpose(mat3(pc.cloudWorld));
	vec3 worldDir = normalize(vWorldPos - frame.cameraPos);
	vec3 cloudDir = cloudWorldInv * worldDir;
	vec3 localDir = normalize(quatRotate(quatConjugate(vInstRot), cloudDir));

	vec3 ddaOrigin = vLocalPos + localDir * 0.0001;
	DdaHit h = traceLocal(ddaOrigin, localDir);

	if (!h.hit) {
		// Discard unhit cube interior — sky pre-pass shows through. Foliage is sparse.
		discard;
	}

	vec4 albedo = texelFetch(palette_sampler, ivec2(h.matIdx, 0), 0);
	vec3 localNormal = -vec3(h.step_sign) * vec3(h.face);

	// Continuous, per-fragment hit point on the voxel's entry face — NOT the
	// voxel center. This is what gives shadow edges their diagonal projection
	// (each fragment's shadow ray starts from a distinct point along the face,
	// so the occluder silhouette projects across the receiver instead of
	// snapping to the receiver-voxel boundary). Same approach the brickmap
	// pillar uses with `h.t` in brickmap_palette_trace.frag.
	vec3 hitInstanceLocal = ddaOrigin + localDir * h.entryT;

	float ao = 1.0;
	if (frame.aoStrength > 0.0) {
		float raw = cornerAO(h.voxel, h.face, h.step_sign, hitInstanceLocal);
		ao = mix(1.0, raw, frame.aoStrength);
	}

	// World-space hit position + normal for the substrate shadow trace.
	vec3 cloudNormal = quatRotate(vInstRot, localNormal);
	vec3 worldNormal = normalize(mat3(pc.cloudWorld) * cloudNormal);
	float NdotL = max(0.0, dot(worldNormal, frame.sunDirection));

	vec3 hitWorld = instanceLocalToWorld(hitInstanceLocal);

	// Substrate shadow query. Bias along the surface normal in world units;
	// slope-scaled term grows at grazing angles where projection foreshortening
	// would otherwise stipple. Skip the receiver's own voxel so a blade
	// doesn't self-occlude its own surface cell.
	//
	// The substrate is built in cloud-local voxel coords, so we pull the cloud's
	// world translation out of the push-constant transform and pass it through
	// — `traceShadowWorld` does the world→cloud-local fold internally. Cloud
	// rotation is invariant=identity per LIGHTING.md §2 so column 3 of
	// pc.cloudWorld is the unscaled translation vector.
	float shadow = 1.0;
	if (frame.shadowsEnabled != 0) {
		float bias = frame.shadowBiasConstant
		           + frame.shadowBiasSlope * (1.0 - clamp(NdotL, 0.0, 1.0));
		vec3 originBiased     = hitWorld + worldNormal * bias;
		vec3 cloudOriginWorld = pc.cloudWorld[3].xyz;
		ivec3 receiverCloudVoxel = ivec3(floor(
			(hitWorld - cloudOriginWorld) / frame.worldVoxelSize));
		// Generous max distance: a few times the cloud diagonal. Past this the
		// substrate DDA bails out as "lit" — a v1-acceptable failure mode for
		// shadows that escape the cloud's extent.
		const float kShadowMaxDist = 64.0;   // world units
		shadow = traceShadowWorld(originBiased, frame.sunDirection,
		                          kShadowMaxDist, frame.worldVoxelSize,
		                          cloudOriginWorld, receiverCloudVoxel);
	}

	vec3 ambient = frame.skyColor * frame.ambientIntensity * ao;
	vec3 direct  = frame.sunColor * frame.sunIntensity * NdotL * shadow;
	vec3 lit = albedo.rgb * (ambient + direct);

	if (frame.debugColor != 0) {
		lit -= vec3(float(h.total_iters) / 250.0);
	}

	outColor = vec4(lit, 1.0);
}
