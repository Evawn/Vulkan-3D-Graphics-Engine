#version 450

layout(location = 0) in vec3 texCoords;

layout(push_constant) uniform PushConstantBlock {
	mat4 NDCtoWorld;
	vec3 cameraPos;
	int maxIterations;
	vec3 skyColor;
	int debugColor;
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

const float PI_OVER_2 = asin(1.0);
const vec4 HORIZON_COLOR = vec4(0.8, 0.9, 1.0, 1.0);

// Cached from header — set once in main()
uvec3 g_volume_size;
uvec3 g_grid_dim;
uint  g_brick_size;
uint  g_grid_cells;

// World-space AABB: longest axis spans [-1, 1]; shorter axes stop short
// proportionally, preserving the model's aspect ratio.
float g_voxel_world_size;   // = 2.0 / max(vs)
vec3  g_half_extents;       // = vec3(vs) * g_voxel_world_size * 0.5

vec3 worldToVoxel(vec3 p) {
	return (p + g_half_extents) / g_voxel_world_size;
}

vec3 voxelToWorld(vec3 p) {
	return p * g_voxel_world_size - g_half_extents;
}

vec4 missColor(vec3 direction) {
	float dotProd = clamp(dot(direction, vec3(0.0, 0.0, 1.0)), -1.0, 1.0);
	float theta = acos(dotProd) / PI_OVER_2;
	vec4 sky = vec4(pc.skyColor, 1.0);
	if (theta < 1.0) return sky * (1.0 - theta) + HORIZON_COLOR * theta;
	else return HORIZON_COLOR * (2.0 - theta);
}

uint brickVoxelMaterial(uint brick_index, ivec3 local) {
	int linear = local.z * 64 + local.y * 8 + local.x;
	int word_idx = linear / 4;
	int byte_lane = linear % 4;
	uint word = bm_data[8 + g_grid_cells + brick_index * 128 + word_idx];
	return (word >> (byte_lane * 8)) & 0xFFu;
}

void main() {
	// Read brickmap header (new 8-uint layout)
	g_volume_size = uvec3(bm_data[0], bm_data[1], bm_data[2]);
	g_brick_size  = bm_data[3];
	g_grid_dim    = uvec3(bm_data[4], bm_data[5], bm_data[6]);
	g_grid_cells  = g_grid_dim.x * g_grid_dim.y * g_grid_dim.z;

	// World AABB derived from per-axis volume — longest axis fills [-1, 1].
	uint max_vs = max(max(g_volume_size.x, g_volume_size.y), g_volume_size.z);
	g_voxel_world_size = 2.0 / float(max_vs);
	g_half_extents     = vec3(g_volume_size) * g_voxel_world_size * 0.5;

	vec3 rayOrigin = pc.cameraPos;
	vec4 transformed = pc.NDCtoWorld * vec4(texCoords, 1.0);
	vec3 pixelLocation = transformed.xyz / transformed.w;
	vec3 direction = normalize(pixelLocation - rayOrigin);
	vec3 invDir = 1.0 / direction;

	// Ray-AABB intersection with the per-axis box [-halfExtents, +halfExtents]
	vec3 tMin = (-g_half_extents - rayOrigin) * invDir;
	vec3 tMax = ( g_half_extents - rayOrigin) * invDir;
	vec3 t1 = min(tMin, tMax);
	vec3 t2 = max(tMin, tMax);
	float tEntry = max(max(t1.x, t1.y), t1.z);
	float tExitVol = min(min(t2.x, t2.y), t2.z);

	if (tEntry > tExitVol || tExitVol < 0.0) {
		outColor = missColor(direction);
		return;
	}

	float t = max(tEntry, 0.0) + 0.0001;
	int total_iters = 0;

	ivec3 grid_dim = ivec3(g_grid_dim);
	int brick_size = int(g_brick_size);
	float brick_size_f = float(brick_size);

	// Convert ray to voxel space for DDA
	vec3 p_voxel = worldToVoxel(rayOrigin + direction * t);
	p_voxel = clamp(p_voxel, vec3(0.001), vec3(g_volume_size) - 0.001);

	// --- Outer DDA setup on the top-level grid ---
	ivec3 cell = ivec3(floor(p_voxel / brick_size_f));
	cell = clamp(cell, ivec3(0), grid_dim - 1);

	// DDA step directions
	ivec3 step_sign = ivec3(
		direction.x >= 0.0 ? 1 : -1,
		direction.y >= 0.0 ? 1 : -1,
		direction.z >= 0.0 ? 1 : -1
	);

	// Distance in t for one full grid cell along each axis.
	// One voxel in world space = g_voxel_world_size, so one brick = bs * voxel_world_size.
	vec3 tDelta = abs(vec3(brick_size_f * g_voxel_world_size) / direction);

	// Distance to the next grid boundary along each axis
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

	// Track which face was crossed for shading
	bvec3 last_face = bvec3(false);

	for (int outer_step = 0; outer_step < pc.maxIterations; outer_step++) {
		// Bounds check: exited the grid (per-axis)
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, grid_dim))) {
			break;
		}

		int grid_idx = cell.x + cell.y * grid_dim.x + cell.z * grid_dim.x * grid_dim.y;
		uint brick_index = bm_data[8 + grid_idx];

		if (brick_index != 0xFFFFFFFFu) {
			// --- Inner DDA: step through 8^3 voxels inside this brick ---
			vec3 brick_origin_voxel = vec3(cell) * brick_size_f;

			// Current position in voxel space at the entry to this cell
			float t_current = max(t, max(tEntry, 0.0));
			vec3 p_inner = worldToVoxel(rayOrigin + direction * t_current);
			vec3 local_f = p_inner - brick_origin_voxel;
			local_f = clamp(local_f, vec3(0.001), vec3(brick_size_f - 0.001));

			ivec3 local_coord = ivec3(floor(local_f));
			local_coord = clamp(local_coord, ivec3(0), ivec3(brick_size - 1));

			// Inner DDA: distance per voxel along each axis (one voxel = voxel_world_size)
			vec3 tDeltaInner = abs(vec3(g_voxel_world_size) / direction);

			// Distance to next voxel boundary
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
				// Bounds check: exited the brick
				if (any(lessThan(local_coord, ivec3(0))) || any(greaterThanEqual(local_coord, ivec3(brick_size)))) {
					total_iters += inner_step;
					break;
				}

				// Test material via packed byte lookup
				uint matIdx = brickVoxelMaterial(brick_index, local_coord);
				if (matIdx != 0u) {
					// HIT -- look up palette color and shade by face direction
					vec4 albedo = texelFetch(palette_sampler, ivec2(matIdx, 0), 0);
					vec4 color = vec4(albedo.rgb * (vec3(1.0) - vec3(inner_face) * 0.1), 1.0);
					if (pc.debugColor != 0) {
						color -= vec4(vec3(float(total_iters + inner_step) / 250.0), 0.0);
					}
					outColor = color;
					return;
				}

				// DDA step: advance along the axis with the smallest tMax
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

		// Outer DDA step: advance to the next grid cell
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

	// No hit: sky color
	vec4 miss = missColor(direction);
	if (pc.debugColor != 0) miss -= vec4(vec3(float(total_iters) / 250.0), 0.0);
	outColor = miss;
}
