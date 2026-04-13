#version 450

layout(location = 0) in vec3 texCoords;

layout(push_constant) uniform PushConstantBlock {
	mat4 NDCtoWorld;
	vec3 cameraPos;
	int maxIterations;
	vec3 skyColor;
	int debugColor;
} pc;

layout(std430, set = 0, binding = 0) readonly buffer BrickmapBuffer {
	uint bm_volume_size;
	uint bm_brick_size;
	uint bm_grid_dim;
	uint bm_brick_count;
	uint bm_top_grid[4096];
	uint bm_brick_data[];   // 128 uints per brick (512 packed material bytes)
};

layout(set = 0, binding = 1) uniform sampler2D palette_sampler;

layout(location = 0) out vec4 outColor;

const float PI_OVER_2 = asin(1.0);
const vec4 HORIZON_COLOR = vec4(0.8, 0.9, 1.0, 1.0);
const vec3 VOLUME_ORIGIN = vec3(-1.0);
const float VOLUME_SCALE = 2.0;

vec3 worldToVoxel(vec3 p) {
	return (p - VOLUME_ORIGIN) / VOLUME_SCALE * float(bm_volume_size);
}

vec3 voxelToWorld(vec3 p) {
	return p / float(bm_volume_size) * VOLUME_SCALE + VOLUME_ORIGIN;
}

vec4 missColor(vec3 direction) {
	float dotProd = clamp(dot(direction, vec3(0.0, 0.0, 1.0)), -1.0, 1.0);
	float theta = acos(dotProd) / PI_OVER_2;
	vec4 sky = vec4(pc.skyColor, 1.0);
	if (theta < 1.0) return sky * (1.0 - theta) + HORIZON_COLOR * theta;
	else return HORIZON_COLOR * (2.0 - theta);
}

// Extract material index from packed brick data
uint brickVoxelMaterial(uint brick_index, ivec3 local) {
	int linear = local.z * 64 + local.y * 8 + local.x;
	int word_idx = linear / 4;
	int byte_lane = linear % 4;
	uint word = bm_brick_data[brick_index * 128 + word_idx];
	return (word >> (byte_lane * 8)) & 0xFFu;
}

void main() {
	vec3 rayOrigin = pc.cameraPos;
	vec4 transformed = pc.NDCtoWorld * vec4(texCoords, 1.0);
	vec3 pixelLocation = transformed.xyz / transformed.w;
	vec3 direction = normalize(pixelLocation - rayOrigin);
	vec3 invDir = 1.0 / direction;

	// Ray-AABB intersection with volume bounds [-1, 1]^3
	vec3 tMin = (-1.0 - rayOrigin) * invDir;
	vec3 tMax = (1.0 - rayOrigin) * invDir;
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

	int grid_dim = int(bm_grid_dim);
	int brick_size = int(bm_brick_size);
	float brick_size_f = float(brick_size);

	// Convert ray to voxel space for DDA
	vec3 p_voxel = worldToVoxel(rayOrigin + direction * t);
	p_voxel = clamp(p_voxel, vec3(0.001), vec3(float(bm_volume_size) - 0.001));

	// --- Outer DDA setup on the top-level grid (16^3) ---
	ivec3 cell = ivec3(floor(p_voxel / brick_size_f));
	cell = clamp(cell, ivec3(0), ivec3(grid_dim - 1));

	// DDA step directions
	ivec3 step_sign = ivec3(
		direction.x >= 0.0 ? 1 : -1,
		direction.y >= 0.0 ? 1 : -1,
		direction.z >= 0.0 ? 1 : -1
	);

	// Distance in t for one full grid cell along each axis
	vec3 tDelta = abs(vec3(brick_size_f) / (direction * float(bm_volume_size) / VOLUME_SCALE));

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
		// Bounds check: exited the grid
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(grid_dim)))) {
			break;
		}

		int grid_idx = cell.x + cell.y * grid_dim + cell.z * grid_dim * grid_dim;
		uint brick_index = bm_top_grid[grid_idx];

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

			// Inner DDA: distance per voxel along each axis
			vec3 tDeltaInner = abs(vec3(1.0) / (direction * float(bm_volume_size) / VOLUME_SCALE));

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
