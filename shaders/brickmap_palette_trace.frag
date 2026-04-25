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
	vec3 sunColor;         // disk tint (multiplied into the returned sky color)
	float sunIntensity;    // scalar brightness of the disk
	float ambientIntensity;// scales skyColor contribution (modulated by AO)
	float aoStrength;      // 0 disables corner AO; 1 = full weight
	int   shadowsEnabled;  // 0/1 — gates the secondary DDA march toward the sun
	int   _pad0;
} pc;

// Flat uint array — per-axis layout:
//   [0]: vs_x   [1]: vs_y   [2]: vs_z   [3]: brick_size
//   [4]: gd_x   [5]: gd_y   [6]: gd_z   [7]: brick_count
//   [8 .. 8+gd_x*gd_y*gd_z-1]: top_grid
//   [8+top_cells ..]:          brick_data (128 uint32 per brick)
layout(std430, set = 0, binding = 0) readonly buffer BrickmapBuffer {
	uint bm_data[];
};

layout(set = 0, binding = 1) uniform sampler2D palette_sampler;

layout(location = 0) out vec4 outColor;

// Cached from header — set once in main()
uvec3 g_volume_size;
uvec3 g_grid_dim;
uint  g_brick_size;
uint  g_grid_cells;

// World-space AABB: longest axis spans [-1, 1]; shorter axes stop short
// proportionally, preserving the model's aspect ratio.
float g_voxel_world_size;   // = 2.0 / max(vs)
vec3  g_half_extents;       // = vec3(vs) * g_voxel_world_size * 0.5

vec3 worldToVoxel(vec3 p) { return (p + g_half_extents) / g_voxel_world_size; }
vec3 voxelToWorld(vec3 p) { return p * g_voxel_world_size - g_half_extents; }

uint brickVoxelMaterial(uint brick_index, ivec3 local) {
	int linear = local.z * 64 + local.y * 8 + local.x;
	int word_idx = linear / 4;
	int byte_lane = linear % 4;
	uint word = bm_data[8 + g_grid_cells + brick_index * 128 + word_idx];
	return (word >> (byte_lane * 8)) & 0xFFu;
}

// Sample a voxel given a brick cell and a (possibly out-of-brick) local offset.
// Crosses into adjacent bricks so AO stays seamless at brick boundaries.
// Returns 0 if outside the grid or inside an empty brick.
uint sampleVoxel(ivec3 brick_cell, ivec3 local) {
	int bs = int(g_brick_size);
	ivec3 actual_cell = brick_cell;
	ivec3 actual_local = local;
	// local offsets here are at most ±1 voxel, so a single normalization pass is enough.
	for (int i = 0; i < 3; i++) {
		if (actual_local[i] < 0)        { actual_cell[i] -= 1; actual_local[i] += bs; }
		else if (actual_local[i] >= bs) { actual_cell[i] += 1; actual_local[i] -= bs; }
	}
	if (any(lessThan(actual_cell, ivec3(0))) ||
	    any(greaterThanEqual(actual_cell, ivec3(g_grid_dim)))) return 0u;
	int gx  = int(g_grid_dim.x);
	int gxy = int(g_grid_dim.x * g_grid_dim.y);
	int grid_idx = actual_cell.x + actual_cell.y * gx + actual_cell.z * gxy;
	uint brick_index = bm_data[8 + grid_idx];
	if (brick_index == 0xFFFFFFFFu) return 0u;
	return brickVoxelMaterial(brick_index, actual_local);
}

// Required by voxel_ao.glsl. Decomposes a flat voxel coord into (brick_cell, local)
// and walks the brickmap; returns true iff the voxel is non-empty.
bool isSolidAt(ivec3 voxelCoord) {
	if (any(lessThan(voxelCoord, ivec3(0))) ||
	    any(greaterThanEqual(voxelCoord, ivec3(g_volume_size)))) return false;
	int bs = int(g_brick_size);
	ivec3 brick_cell = voxelCoord / bs;
	ivec3 local = voxelCoord - brick_cell * bs;
	int gx  = int(g_grid_dim.x);
	int gxy = int(g_grid_dim.x * g_grid_dim.y);
	int grid_idx = brick_cell.x + brick_cell.y * gx + brick_cell.z * gxy;
	uint brick_index = bm_data[8 + grid_idx];
	if (brick_index == 0xFFFFFFFFu) return false;
	return brickVoxelMaterial(brick_index, local) != 0u;
}

#include "sky.glsl"
#include "voxel_ao.glsl"
#include "lighting.glsl"

struct Hit {
	bool  hit;
	float t;
	ivec3 brick_cell;
	ivec3 local;
	bvec3 face;        // entry face axis (one of three true)
	ivec3 step_sign;   // sign of ray direction per axis
	uint  matIdx;
	int   total_iters;
};

// Two-level DDA. Returns closest-hit info (which, for opaque voxels, is also
// the first hit). Shared by the primary ray and shadow rays.
Hit trace(vec3 rayOrigin, vec3 direction) {
	Hit h;
	h.hit = false;
	h.t = 0.0;
	h.brick_cell = ivec3(0);
	h.local = ivec3(0);
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

	ivec3 grid_dim = ivec3(g_grid_dim);
	int brick_size = int(g_brick_size);
	float brick_size_f = float(brick_size);

	vec3 p_voxel = worldToVoxel(rayOrigin + direction * t);
	p_voxel = clamp(p_voxel, vec3(0.001), vec3(g_volume_size) - 0.001);

	ivec3 cell = ivec3(floor(p_voxel / brick_size_f));
	cell = clamp(cell, ivec3(0), grid_dim - 1);

	ivec3 step_sign = ivec3(
		direction.x >= 0.0 ? 1 : -1,
		direction.y >= 0.0 ? 1 : -1,
		direction.z >= 0.0 ? 1 : -1
	);
	h.step_sign = step_sign;

	vec3 tDelta = abs(vec3(brick_size_f * g_voxel_world_size) / direction);

	vec3 cell_world_min = voxelToWorld(vec3(cell) * brick_size_f);
	vec3 cell_world_max = voxelToWorld(vec3(cell + 1) * brick_size_f);
	vec3 tMaxOuter = vec3(
		direction.x >= 0.0 ? (cell_world_max.x - rayOrigin.x) * invDir.x
		                    : (cell_world_min.x - rayOrigin.x) * invDir.x,
		direction.y >= 0.0 ? (cell_world_max.y - rayOrigin.y) * invDir.y
		                    : (cell_world_min.y - rayOrigin.y) * invDir.y,
		direction.z >= 0.0 ? (cell_world_max.z - rayOrigin.z) * invDir.z
		                    : (cell_world_min.z - rayOrigin.z) * invDir.z
	);

	bvec3 last_face = bvec3(false);
	int total_iters = 0;

	for (int outer_step = 0; outer_step < pc.maxIterations; outer_step++) {
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, grid_dim))) break;

		int grid_idx = cell.x + cell.y * grid_dim.x + cell.z * grid_dim.x * grid_dim.y;
		uint brick_index = bm_data[8 + grid_idx];

		if (brick_index != 0xFFFFFFFFu) {
			vec3 brick_origin_voxel = vec3(cell) * brick_size_f;
			float t_current = max(t, max(tEntry, 0.0));
			vec3 p_inner = worldToVoxel(rayOrigin + direction * t_current);
			vec3 local_f = p_inner - brick_origin_voxel;
			local_f = clamp(local_f, vec3(0.001), vec3(brick_size_f - 0.001));

			ivec3 local_coord = ivec3(floor(local_f));
			local_coord = clamp(local_coord, ivec3(0), ivec3(brick_size - 1));

			vec3 tDeltaInner = abs(vec3(g_voxel_world_size) / direction);

			vec3 voxel_world_min = voxelToWorld(brick_origin_voxel + vec3(local_coord));
			vec3 voxel_world_max = voxelToWorld(brick_origin_voxel + vec3(local_coord + 1));
			vec3 tMaxInner = vec3(
				direction.x >= 0.0 ? (voxel_world_max.x - rayOrigin.x) * invDir.x
				                    : (voxel_world_min.x - rayOrigin.x) * invDir.x,
				direction.y >= 0.0 ? (voxel_world_max.y - rayOrigin.y) * invDir.y
				                    : (voxel_world_min.y - rayOrigin.y) * invDir.y,
				direction.z >= 0.0 ? (voxel_world_max.z - rayOrigin.z) * invDir.z
				                    : (voxel_world_min.z - rayOrigin.z) * invDir.z
			);

			bvec3 inner_face = last_face;
			int max_inner_steps = brick_size * 3;

			for (int inner_step = 0; inner_step < max_inner_steps; inner_step++) {
				if (any(lessThan(local_coord, ivec3(0))) || any(greaterThanEqual(local_coord, ivec3(brick_size)))) {
					total_iters += inner_step;
					break;
				}

				uint matIdx = brickVoxelMaterial(brick_index, local_coord);
				if (matIdx != 0u) {
					// Compute t at the entry face of the hit voxel. We recompute from
					// the voxel's world bounds rather than subtracting tDeltaInner so
					// the no-step case (hit on first voxel of brick) stays correct.
					vec3 vmin = voxelToWorld(brick_origin_voxel + vec3(local_coord));
					vec3 vmax = voxelToWorld(brick_origin_voxel + vec3(local_coord + 1));
					float t_hit = t_current;
					if      (inner_face.x) t_hit = ((step_sign.x > 0 ? vmin.x : vmax.x) - rayOrigin.x) * invDir.x;
					else if (inner_face.y) t_hit = ((step_sign.y > 0 ? vmin.y : vmax.y) - rayOrigin.y) * invDir.y;
					else if (inner_face.z) t_hit = ((step_sign.z > 0 ? vmin.z : vmax.z) - rayOrigin.z) * invDir.z;

					h.hit = true;
					h.t = t_hit;
					h.brick_cell = cell;
					h.local = local_coord;
					h.face = inner_face;
					h.matIdx = matIdx;
					h.total_iters = total_iters + inner_step;
					return h;
				}

				if (tMaxInner.x < tMaxInner.y && tMaxInner.x < tMaxInner.z) {
					local_coord.x += step_sign.x;
					tMaxInner.x += tDeltaInner.x;
					inner_face = bvec3(true, false, false);
				} else if (tMaxInner.y < tMaxInner.z) {
					local_coord.y += step_sign.y;
					tMaxInner.y += tDeltaInner.y;
					inner_face = bvec3(false, true, false);
				} else {
					local_coord.z += step_sign.z;
					tMaxInner.z += tDeltaInner.z;
					inner_face = bvec3(false, false, true);
				}
			}
		}

		total_iters++;

		last_face = bvec3(false);
		if (tMaxOuter.x < tMaxOuter.y && tMaxOuter.x < tMaxOuter.z) {
			t = tMaxOuter.x;
			tMaxOuter.x += tDelta.x;
			cell.x += step_sign.x;
			last_face = bvec3(true, false, false);
		} else if (tMaxOuter.y < tMaxOuter.z) {
			t = tMaxOuter.y;
			tMaxOuter.y += tDelta.y;
			cell.y += step_sign.y;
			last_face = bvec3(false, true, false);
		} else {
			t = tMaxOuter.z;
			tMaxOuter.z += tDelta.z;
			cell.z += step_sign.z;
			last_face = bvec3(false, false, true);
		}
	}

	h.total_iters = total_iters;
	return h;
}

void main() {
	// Read brickmap header (8-uint layout)
	g_volume_size = uvec3(bm_data[0], bm_data[1], bm_data[2]);
	g_brick_size  = bm_data[3];
	g_grid_dim    = uvec3(bm_data[4], bm_data[5], bm_data[6]);
	g_grid_cells  = g_grid_dim.x * g_grid_dim.y * g_grid_dim.z;

	uint max_vs = max(max(g_volume_size.x, g_volume_size.y), g_volume_size.z);
	g_voxel_world_size = 2.0 / float(max_vs);
	g_half_extents     = vec3(g_volume_size) * g_voxel_world_size * 0.5;

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

	// Corner AO (skip the lookups entirely when disabled)
	float ao = 1.0;
	if (pc.aoStrength > 0.0) {
		ivec3 hitVoxel = h.brick_cell * int(g_brick_size) + h.local;
		float raw = cornerAO(hitVoxel, h.face, h.step_sign, hitPos);
		ao = mix(1.0, raw, pc.aoStrength);
	}

	// Directional diffuse + hard shadow ray
	float NdotL = max(0.0, dot(normal, pc.sunDirection));
	float shadow = 1.0;
	int shadow_iters = 0;
	if (pc.shadowsEnabled != 0 && NdotL > 0.0) {
		// Offset along the normal by a small fraction of a voxel to avoid
		// self-intersection with the hit voxel's face.
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
