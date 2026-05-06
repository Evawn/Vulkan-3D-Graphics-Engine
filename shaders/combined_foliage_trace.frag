#version 450

// CombinedRenderer — foliage trace pass.
//
// Forked from instanced_voxel.frag. Differences:
//   1. Sun shadow ray uses the new `traceShadowWorld` (shadow_trace.glsl)
//      that walks the *shadow occupancy brickmap* with two-level DDA.
//      Both terrain occlusion and animated foliage occlusion are pre-baked
//      into that brickmap (terrain at terrain-bake time; foliage by the
//      shadow_foliage_write compute pass each frame).
//   2. Coordinate convention assumes the foliage cloud is anchored at world
//      origin (cloudOriginWorld = vec3(0)).

layout(location = 0) in vec3 vLocalPos;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) flat in int   vFrameIdx;
layout(location = 3) flat in vec4  vInstRot;
layout(location = 4) flat in vec3  vInstPos;
layout(location = 5) flat in float vInstScale;

layout(push_constant) uniform DrawPushConstantBlock {
	mat4 cloudWorld;
	vec3 aabbMin; float _pad0;
	vec3 aabbMax; float _pad1;
} pc;

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

layout(set = 0, binding = 1) uniform usampler2DArray volume_sampler;
layout(set = 0, binding = 2) uniform sampler2D  palette_sampler;
layout(set = 0, binding = 3) uniform VolumeMeta {
	ivec3 size;
	int   frameCount;
} meta;

// Per-frame state — mirror of CombinedFrameUbo on the host. Same struct as
// the terrain trace shader binds.
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
	float worldVoxelSize;
	int   maxShadowBrickSteps;
	float _ubo_pad1;
} frame;

// Shadow occupancy brickmap (shadow_trace.glsl reads `shadowBM`).
layout(std430, set = 0, binding = 5) readonly buffer ShadowBrickmapBuffer {
	uint data[];
} shadowBM;

layout(location = 0) out vec4 outColor;

// shadow_trace.glsl reads this name. Set in main() from the FrameUbo.
int frame_maxShadowBrickSteps;

vec4 quatConjugate(vec4 q) { return vec4(-q.xyz, q.w); }
vec3 quatRotate(vec4 q, vec3 v) {
	vec3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

uint sampleMaterialAtFrame(ivec3 voxelCoord, int f) {
	if (any(lessThan(voxelCoord, ivec3(0))) ||
	    any(greaterThanEqual(voxelCoord, meta.size))) return 0u;
	ivec3 c = ivec3(voxelCoord.x, voxelCoord.y + voxelCoord.z * meta.size.y, f);
	return texelFetch(volume_sampler, c, 0).r;
}

uint sampleMaterial(ivec3 voxelCoord) {
	return sampleMaterialAtFrame(voxelCoord, vFrameIdx);
}

bool isSolidAt(ivec3 voxelCoord) {
	return sampleMaterial(voxelCoord) != 0u;
}

vec3  g_voxel_local;
vec3  g_local_origin;
int   uMaxIterations;

vec3 localToVoxel(vec3 p) { return (p - g_local_origin) / g_voxel_local; }
vec3 worldToVoxel(vec3 p) { return localToVoxel(p); }

#include "voxel_ao.glsl"
#include "instanced_voxel_dda.glsl"
#include "shadow_trace.glsl"

vec3 instanceLocalToWorld(vec3 pLocal) {
	vec3 inCloud = quatRotate(vInstRot, pLocal * vInstScale) + vInstPos;
	return (pc.cloudWorld * vec4(inCloud, 1.0)).xyz;
}

void main() {
	frame_maxShadowBrickSteps = frame.maxShadowBrickSteps;

	g_local_origin = pc.aabbMin;
	g_voxel_local  = (pc.aabbMax - pc.aabbMin) / vec3(meta.size);
	uMaxIterations = frame.maxIterations;

	// Ray direction in instance-local AABB space. Magnitude doesn't matter
	// for direction; transpose(scale) preserves it up to a uniform factor
	// that normalize() removes.
	mat3 cloudWorldInv = transpose(mat3(pc.cloudWorld));
	vec3 worldDir = normalize(vWorldPos - frame.cameraPos);
	vec3 cloudDir = cloudWorldInv * worldDir;
	vec3 localDir = normalize(quatRotate(quatConjugate(vInstRot), cloudDir));

	// Camera position in instance-local AABB space. cloudWorld is a uniform
	// scale (kWorldVoxelSize) anchored at world origin, so its inverse on
	// positions is 1 / cloudWorld[0][0]. Then subtract the per-instance
	// translation, inverse-rotate by the instance quaternion, and divide by
	// the per-instance scale (always 1.0 by VISION.md §1.1, but kept for
	// shader parity with the vertex transform).
	float invCloudScale = 1.0 / pc.cloudWorld[0][0];
	vec3 cameraInCloud  = frame.cameraPos * invCloudScale;
	vec3 cameraLocal    = quatRotate(quatConjugate(vInstRot),
	                                 cameraInCloud - vInstPos) / vInstScale;

	// Ray-AABB clip — same fix shape as voxel_preview.frag in commit
	// 6a2f8e6 ("voxel preview invisible inside volume"). When the camera
	// is INSIDE the cube the rasterized fragment's vLocalPos is the back
	// face (the ray's exit), not the entry. Starting DDA there would walk
	// the ray straight out of the volume on iteration 0, so we always
	// start the DDA at the actual entry: tEntry for outside, the camera
	// itself for inside (tEntry < 0 → tOrigin = 0).
	vec3 invDir = 1.0 / localDir;
	vec3 tNear  = (pc.aabbMin - cameraLocal) * invDir;
	vec3 tFar   = (pc.aabbMax - cameraLocal) * invDir;
	vec3 t1     = min(tNear, tFar);
	vec3 t2     = max(tNear, tFar);
	float tEntry = max(max(t1.x, t1.y), t1.z);
	float tExit  = min(min(t2.x, t2.y), t2.z);

	// With NoCullFill (so the cube still rasterizes when the camera is
	// inside, where every face is back-facing), each pixel runs main()
	// twice when the camera is OUTSIDE — once for the entry face, once
	// for the exit face. Discard the past-midpoint duplicate so the DDA
	// runs once per pixel. tEntry > 0 guard keeps the inside-camera
	// fragment alive (its tHere ≈ tExit, which would otherwise discard).
	float tHere = dot(vLocalPos - cameraLocal, localDir);
	if (tEntry > 0.0 && tHere > 0.5 * (tEntry + tExit)) discard;

	float tOrigin = max(tEntry, 0.0);
	vec3 ddaOrigin = cameraLocal + localDir * (tOrigin + 0.0001);

	DdaHit h = traceLocal(ddaOrigin, localDir);

	if (!h.hit) {
		discard;
	}

	vec4 albedo = texelFetch(palette_sampler, ivec2(h.matIdx, 0), 0);
	vec3 localNormal = -vec3(h.step_sign) * vec3(h.face);

	vec3 hitInstanceLocal = ddaOrigin + localDir * h.entryT;

	float ao = 1.0;
	if (frame.aoStrength > 0.0) {
		float raw = cornerAO(h.voxel, h.face, h.step_sign, hitInstanceLocal);
		ao = mix(1.0, raw, frame.aoStrength);
	}

	vec3 cloudNormal = quatRotate(vInstRot, localNormal);
	vec3 worldNormal = normalize(mat3(pc.cloudWorld) * cloudNormal);
	float NdotL = max(0.0, dot(worldNormal, frame.sunDirection));

	vec3 hitWorld = instanceLocalToWorld(hitInstanceLocal);

	float shadow = 1.0;
	if (frame.shadowsEnabled != 0) {
		// Receiver's own WORLD voxel — exempt from the shadow query.
		// Cloud is anchored at world origin in CombinedRenderer, so cloud-
		// local voxel == world voxel. Integer-grid skip is the only self-
		// occlusion guard needed; no bias on hitWorld.
		ivec3 receiverWorldVoxel = ivec3(floor(hitWorld / frame.worldVoxelSize));
		const float kShadowMaxDist = 64.0;
		shadow = traceShadowWorld(hitWorld, frame.sunDirection,
		                          kShadowMaxDist, frame.worldVoxelSize,
		                          receiverWorldVoxel);
	}

	vec3 ambient = frame.skyColor * frame.ambientIntensity * ao;
	vec3 direct  = frame.sunColor * frame.sunIntensity * NdotL * shadow;
	vec3 lit = albedo.rgb * (ambient + direct);

	if (frame.debugColor != 0) {
		lit -= vec3(float(h.total_iters) / 250.0);
	}

	outColor = vec4(lit, 1.0);

	// Write the actual voxel-hit depth so foliage cubes z-test against terrain
	// at the voxel surface, not at the bounding-box face.
	vec4 clipPos = frame.viewProj * vec4(hitWorld, 1.0);
	gl_FragDepth = clipPos.z / clipPos.w;
}
