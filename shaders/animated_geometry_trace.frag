#version 450

layout(location = 0) in vec3 texCoords;

layout(push_constant) uniform PushConstantBlock {
	mat4 NDCtoWorld;
	vec3 cameraPos;
	int maxIterations;
	vec3 skyColor;
	int debugColor;
	vec3 sunDirection;     // normalized, world-space (from origin toward sun)
	float sunCosHalfAngle; // cos(half apparent-size) — ray is inside disk when dot > this
	vec3 sunColor;         // disk tint
	float sunIntensity;    // scalar brightness of the disk
	ivec3 volumeSize;
	float ambientIntensity;// scales skyColor contribution (modulated by AO)
	float aoStrength;      // 0 disables corner AO; 1 = full weight
	int   shadowsEnabled;  // 0/1 — gates the secondary DDA march toward the sun
	int   _pad0;
	int   _pad1;
} pc;

layout(set = 0, binding = 0) uniform usampler3D volume_sampler;
layout(set = 0, binding = 1) uniform sampler2D palette_sampler;

layout(location = 0) out vec4 outColor;

// World-space AABB: longest axis spans [-1, 1]; shorter axes stop short
// proportionally, preserving the volume's aspect ratio.
float g_voxel_world_size;   // = 2.0 / max(volumeSize)
vec3  g_half_extents;       // = vec3(volumeSize) * g_voxel_world_size * 0.5

vec3 worldToVoxel(vec3 p) { return (p + g_half_extents) / g_voxel_world_size; }
vec3 voxelToWorld(vec3 p) { return p * g_voxel_world_size - g_half_extents; }

uint sampleMaterial(ivec3 voxelCoord) {
	if (any(lessThan(voxelCoord, ivec3(0))) ||
	    any(greaterThanEqual(voxelCoord, pc.volumeSize))) return 0u;
	return texelFetch(volume_sampler, voxelCoord, 0).r;
}

// Required by voxel_ao.glsl.
bool isSolidAt(ivec3 voxelCoord) {
	return sampleMaterial(voxelCoord) != 0u;
}

#include "sky.glsl"
#include "voxel_ao.glsl"
#include "lighting.glsl"

struct Hit {
	bool  hit;
	float t;
	ivec3 voxel;
	bvec3 face;
	ivec3 step_sign;
	uint  matIdx;
	int   total_iters;
};

// Single-level DDA over the flat voxel grid.
Hit trace(vec3 rayOrigin, vec3 direction) {
	Hit h;
	h.hit = false;
	h.t = 0.0;
	h.voxel = ivec3(0);
	h.face = bvec3(false);
	h.step_sign = ivec3(0);
	h.matIdx = 0u;
	h.total_iters = 0;

	vec3 invDir = 1.0 / direction;

	// Ray-AABB clip against the volume box.
	vec3 tMin = (-g_half_extents - rayOrigin) * invDir;
	vec3 tMax = ( g_half_extents - rayOrigin) * invDir;
	vec3 t1v = min(tMin, tMax);
	vec3 t2v = max(tMin, tMax);
	float tEntry = max(max(t1v.x, t1v.y), t1v.z);
	float tExitVol = min(min(t2v.x, t2v.y), t2v.z);
	if (tEntry > tExitVol || tExitVol < 0.0) return h;

	float t = max(tEntry, 0.0) + 0.0001;

	vec3 p_voxel = worldToVoxel(rayOrigin + direction * t);
	p_voxel = clamp(p_voxel, vec3(0.001), vec3(pc.volumeSize) - 0.001);

	ivec3 voxel = ivec3(floor(p_voxel));
	voxel = clamp(voxel, ivec3(0), pc.volumeSize - 1);

	ivec3 step_sign = ivec3(
		direction.x >= 0.0 ? 1 : -1,
		direction.y >= 0.0 ? 1 : -1,
		direction.z >= 0.0 ? 1 : -1
	);
	h.step_sign = step_sign;

	vec3 tDelta = abs(vec3(g_voxel_world_size) / direction);

	vec3 vmin = voxelToWorld(vec3(voxel));
	vec3 vmax = voxelToWorld(vec3(voxel + 1));
	vec3 tMaxAxis = vec3(
		direction.x >= 0.0 ? (vmax.x - rayOrigin.x) * invDir.x
		                    : (vmin.x - rayOrigin.x) * invDir.x,
		direction.y >= 0.0 ? (vmax.y - rayOrigin.y) * invDir.y
		                    : (vmin.y - rayOrigin.y) * invDir.y,
		direction.z >= 0.0 ? (vmax.z - rayOrigin.z) * invDir.z
		                    : (vmin.z - rayOrigin.z) * invDir.z
	);

	bvec3 last_face = bvec3(false);

	for (int i = 0; i < pc.maxIterations; i++) {
		if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, pc.volumeSize))) break;

		uint matIdx = sampleMaterial(voxel);
		if (matIdx != 0u) {
			// t at the voxel's entry face (recompute from world bounds so the
			// no-step case stays correct).
			vec3 fmin = voxelToWorld(vec3(voxel));
			vec3 fmax = voxelToWorld(vec3(voxel + 1));
			float t_hit = t;
			if      (last_face.x) t_hit = ((step_sign.x > 0 ? fmin.x : fmax.x) - rayOrigin.x) * invDir.x;
			else if (last_face.y) t_hit = ((step_sign.y > 0 ? fmin.y : fmax.y) - rayOrigin.y) * invDir.y;
			else if (last_face.z) t_hit = ((step_sign.z > 0 ? fmin.z : fmax.z) - rayOrigin.z) * invDir.z;

			h.hit = true;
			h.t = t_hit;
			h.voxel = voxel;
			h.face = last_face;
			h.matIdx = matIdx;
			h.total_iters = i;
			return h;
		}

		last_face = bvec3(false);
		if (tMaxAxis.x < tMaxAxis.y && tMaxAxis.x < tMaxAxis.z) {
			t = tMaxAxis.x;
			tMaxAxis.x += tDelta.x;
			voxel.x += step_sign.x;
			last_face = bvec3(true, false, false);
		} else if (tMaxAxis.y < tMaxAxis.z) {
			t = tMaxAxis.y;
			tMaxAxis.y += tDelta.y;
			voxel.y += step_sign.y;
			last_face = bvec3(false, true, false);
		} else {
			t = tMaxAxis.z;
			tMaxAxis.z += tDelta.z;
			voxel.z += step_sign.z;
			last_face = bvec3(false, false, true);
		}

		h.total_iters = i + 1;
	}

	return h;
}

void main() {
	int max_vs = max(max(pc.volumeSize.x, pc.volumeSize.y), pc.volumeSize.z);
	g_voxel_world_size = 2.0 / float(max_vs);
	g_half_extents     = vec3(pc.volumeSize) * g_voxel_world_size * 0.5;

	vec3 rayOrigin = pc.cameraPos;
	vec4 transformed = pc.NDCtoWorld * vec4(texCoords, 1.0);
	vec3 pixelLocation = transformed.xyz / transformed.w;
	vec3 direction = normalize(pixelLocation - rayOrigin);

	Hit h = trace(rayOrigin, direction);

	if (!h.hit) {
		vec4 miss = missColor(direction);
		if (pc.debugColor != 0) miss -= vec4(vec3(float(h.total_iters) / 250.0), 0.0);
		outColor = miss;
		return;
	}

	vec4 albedo = texelFetch(palette_sampler, ivec2(h.matIdx, 0), 0);
	vec3 normal = -vec3(h.step_sign) * vec3(h.face);
	vec3 hitPos = rayOrigin + direction * h.t;

	float ao = 1.0;
	if (pc.aoStrength > 0.0) {
		float raw = cornerAO(h.voxel, h.face, h.step_sign, hitPos);
		ao = mix(1.0, raw, pc.aoStrength);
	}

	float NdotL = max(0.0, dot(normal, pc.sunDirection));
	float shadow = 1.0;
	int shadow_iters = 0;
	if (pc.shadowsEnabled != 0 && NdotL > 0.0) {
		vec3 shadowOrigin = hitPos + normal * (g_voxel_world_size * 0.01);
		Hit sh = trace(shadowOrigin, pc.sunDirection);
		shadow = sh.hit ? 0.0 : 1.0;
		shadow_iters = sh.total_iters;
	}

	vec4 color = vec4(shadeLit(albedo.rgb, ao, NdotL, shadow), 1.0);
	if (pc.debugColor != 0) {
		color -= vec4(vec3(float(h.total_iters + shadow_iters) / 250.0), 0.0);
	}
	outColor = color;
}
