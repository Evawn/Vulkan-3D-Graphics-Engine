#version 450

layout(location = 0) in vec3 texCoords;

layout(push_constant) uniform PushConstantBlock {
	mat4 NDCtoWorld;
	vec3 cameraPos;
	int maxIterations;
	vec3 skyColor;
	int debugColor;
} pc;

layout(set = 0, binding = 0) uniform sampler3D brick_sampler;

layout(std430, set = 0, binding = 1) readonly buffer SVOBuffer {
	uint svo_num_levels;
	uint svo_volume_size;
	uint svo_leaf_size;
	uint svo_node_count;
	uint svo_nodes[];
};

layout(location = 0) out vec4 outColor;

const float PI_OVER_2 = asin(1.0);
const vec4 HORIZON_COLOR = vec4(0.8, 0.9, 1.0, 1.0);
const vec3 VOLUME_ORIGIN = vec3(-1.0);
const float VOLUME_SCALE = 2.0;

vec3 worldToVoxel(vec3 p) {
	return (p - VOLUME_ORIGIN) / VOLUME_SCALE * float(svo_volume_size);
}

vec3 voxelToWorld(vec3 p) {
	return p / float(svo_volume_size) * VOLUME_SCALE + VOLUME_ORIGIN;
}

vec4 missColor(vec3 direction) {
	float dotProd = clamp(dot(direction, vec3(0.0, 0.0, 1.0)), -1.0, 1.0);
	float theta = acos(dotProd) / PI_OVER_2;
	vec4 sky = vec4(pc.skyColor, 1.0);
	if (theta < 1.0) return sky * (1.0 - theta) + HORIZON_COLOR * theta;
	else return HORIZON_COLOR * (2.0 - theta);
}

// Descend the SVO to find the leaf/empty region containing point p (in voxel space).
// Returns true if the region is occupied (has voxel data), false if empty.
// leaf_min: voxel-space origin of the region
// leaf_sz: side length of the region in voxels
bool descendTree(vec3 p, out vec3 leaf_min, out float leaf_sz) {
	vec3 node_min = vec3(0.0);
	float node_sz = float(svo_volume_size);
	uint node_idx = 0;
	uint levels = min(svo_num_levels, 16u);

	for (uint i = 0; i < levels; i++) {
		float half_sz = node_sz * 0.5;
		ivec3 bits = ivec3(greaterThanEqual(p, node_min + half_sz));
		int octant = bits.x | (bits.y << 1) | (bits.z << 2);

		uint data = svo_nodes[node_idx];
		uint mask = data & 0xFFu;

		if ((mask & (1u << octant)) == 0u) {
			// Empty octant at this level
			leaf_min = node_min + vec3(bits) * half_sz;
			leaf_sz = half_sz;
			return false;
		}

		node_min += vec3(bits) * half_sz;
		node_sz = half_sz;

		if (i < levels - 1u) {
			uint first_child = data >> 8;
			uint child_offset = bitCount(mask & ((1u << octant) - 1u));
			node_idx = first_child + child_offset;
		}
	}

	leaf_min = node_min;
	leaf_sz = node_sz;
	return true;
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

	// Start traversal at the volume entry point (or camera if inside)
	float t = max(tEntry, 0.0) + 0.0001;
	int total_iters = 0;
	bvec3 last_face = bvec3(tEntry == t1.x, tEntry == t1.y, tEntry == t1.z);
	bvec3 advance = bvec3(direction.x >= 0.0, direction.y >= 0.0, direction.z >= 0.0);

	for (int step = 0; step < pc.maxIterations; step++) {
		if (t >= tExitVol) break;

		// Current point in voxel space
		vec3 p_voxel = worldToVoxel(rayOrigin + direction * t);
		p_voxel = clamp(p_voxel, vec3(0.001), vec3(float(svo_volume_size) - 0.001));

		// Descend SVO to find which node/leaf we're in
		vec3 leaf_min;
		float leaf_sz;
		bool occupied = descendTree(p_voxel, leaf_min, leaf_sz);

		if (occupied) {
			// ---- Leaf DDA: ray-march within the occupied leaf chunk ----
			int leaf_dim = int(leaf_sz);
			int max_leaf_iters = leaf_dim * 3;

			vec3 local_point = p_voxel - leaf_min;
			ivec3 local_coord = ivec3(floor(local_point));
			local_coord = clamp(local_coord, ivec3(0), ivec3(leaf_dim - 1));

			bvec3 step_dir = last_face;

			for (int i = 0; i < max_leaf_iters; i++) {
				// Bounds check: exited the leaf
				if (any(lessThan(local_coord, ivec3(0))) || any(greaterThanEqual(local_coord, ivec3(leaf_dim)))) {
					total_iters += i;
					break;
				}

				// Sample the original 3D texture at global coordinates
				ivec3 global_coord = ivec3(leaf_min) + local_coord;
				vec4 voxel = texelFetch(brick_sampler, global_coord, 0);
				if (voxel != vec4(0.0)) {
					// Hit: shade by face direction
					vec4 color = vec4(vec3(1.0) - vec3(step_dir) * 0.1, 1.0);
					if (pc.debugColor != 0) color -= vec4(vec3(float(total_iters + i) / 250.0), 0.0);
					outColor = color;
					return;
				}

				// DDA step to next voxel
				vec3 tMin_l = (vec3(local_coord) - local_point) * invDir;
				vec3 tMax_l = (vec3(local_coord) + 1.0 - local_point) * invDir;
				vec3 t1_l = min(tMin_l, tMax_l);
				vec3 t2_l = max(tMin_l, tMax_l);
				float tExit_l = min(min(t2_l.x, t2_l.y), t2_l.z);
				step_dir = bvec3(tExit_l == t2_l.x, tExit_l == t2_l.y, tExit_l == t2_l.z);
				local_coord += ivec3(step_dir) * (ivec3(-1) + 2 * ivec3(advance));
				local_point += direction * tExit_l;
			}
		}

		total_iters++;

		// Advance t past this node's AABB
		vec3 leaf_w_min = voxelToWorld(leaf_min);
		vec3 leaf_w_max = voxelToWorld(leaf_min + leaf_sz);
		vec3 t_lo = (leaf_w_min - rayOrigin) * invDir;
		vec3 t_hi = (leaf_w_max - rayOrigin) * invDir;
		vec3 t_far = max(t_lo, t_hi);
		float t_node_exit = min(min(t_far.x, t_far.y), t_far.z);
		last_face = bvec3(t_node_exit == t_far.x, t_node_exit == t_far.y, t_node_exit == t_far.z);
		t = t_node_exit + 0.0001;
	}

	// No hit: sky color
	vec4 miss = missColor(direction);
	if (pc.debugColor != 0) miss -= vec4(vec3(float(total_iters) / 250.0), 0.0);
	outColor = miss;
}
