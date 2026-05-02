#version 450

// CombinedRenderer — terrain trace pass.
//
// Forked from brickmap_palette_trace.frag. Two material differences:
//   1. Sun shadow ray uses the unified `traceShadowWorld` (substrate.glsl)
//      with SUBSTRATE_TERRAIN defined, so shadows correctly OR foliage and
//      terrain occluders. Replaces the inline secondary `trace(...)` walk
//      that existed in the standalone shader.
//   2. Shading state (sun/sky/time/AO/etc.) lives in a shared FrameUbo
//      instead of the per-draw push constant — both this shader and the
//      foliage trace shader bind the same UBO, so per-frame writes happen
//      once.
//
// Coordinate convention: terrain is centered at world origin, same as the
// standalone shader. The substrate shadow query receives `terrainOriginVoxel`
// = `-floor(volumeSize / 2)` so it can address terrain bricks via world-
// voxel coords.

layout(location = 0) in vec3 texCoords;

// Per-draw push constant — primary-trace geometry only. All shading state
// lives in the FrameUbo; everything that varies per draw is here.
layout(push_constant) uniform PushConstantBlock {
	mat4  ndcToWorld;
	ivec3 terrainOriginVoxel;   int _pad0;
} pc;

// --- Bindings ---
// Layout matches CombinedRenderer's BindingTable. The descriptor numbering is
// shared between this pass and the foliage trace pass for the substrate-side
// resources (so future shader edits stay aligned).

// 0: Terrain brickmap. Symbol name `terrain` is required by terrain_brickmap.glsl.
layout(std430, set = 0, binding = 0) readonly buffer TerrainBrickmapBuffer {
	uint data[];
} terrain;

// 1: Palette texture.
layout(set = 0, binding = 1) uniform sampler2D palette_sampler;

// 2: Per-frame state (mirrors CombinedFrameUbo on the host).
layout(set = 0, binding = 2) uniform FrameUbo {
	mat4  viewProj;
	mat4  ndcToWorld;          // unused here (PC has it); shared layout with foliage shader
	vec3  cameraPos;           int   maxIterations;
	vec3  skyColor;            int   debugColor;
	vec3  sunDirection;        float sunCosHalfAngle;
	vec3  sunColor;            float sunIntensity;
	float ambientIntensity;
	float aoStrength;
	int   shadowsEnabled;
	float time;
	int   frameCount;
	float worldVoxelSize;
	float _ubo_pad0;
	float _ubo_pad1;
	ivec3 terrainOriginVoxel;  // mirror of pc.terrainOriginVoxel — substrate.glsl reads this name
	int   _ubo_pad2;
} frame;

// Per-asset metadata for the foliage cloud (substrate.glsl reads `meta.size`,
// `meta.frameCount`). Even though this is the terrain pass, we need foliage
// metadata for the substrate's shadow query.
layout(set = 0, binding = 3) uniform VolumeMeta {
	ivec3 size;
	int   frameCount;
} meta;

// Foliage substrate query inputs (substrate.glsl).
struct InstanceData {
	vec3  position;       float scale;
	vec4  rotation;
	float animOffset;
	float _pad0;
	int   yawIdx;
	float _pad2;
};
layout(std430, set = 0, binding = 4) readonly buffer InstanceBuffer {
	InstanceData instances[];
} ib;

layout(std430, set = 0, binding = 5) readonly buffer SubstrateBuffer {
	uint data[];
} substrate;

layout(std430, set = 0, binding = 6) readonly buffer BitmaskBuffer {
	uint bits[];
} bitmask;

layout(location = 0) out vec4 outColor;

// terrainOriginVoxel is consumed by name from terrain_brickmap.glsl. Read out
// of the FrameUbo (mirrored from the push constant for substrate.glsl's
// shadow query, which only sees the FrameUbo).
ivec3 terrainOriginVoxel;

// Cached from header — set once in main()
uvec3 g_volume_size;
uvec3 g_grid_dim;
uint  g_brick_size;
uint  g_grid_cells;

// World-space AABB: terrain centered at world origin (longest axis spans
// [-halfExtents, halfExtents]).
float g_voxel_world_size;
vec3  g_half_extents;

vec3 worldToVoxel(vec3 p) { return (p + g_half_extents) / g_voxel_world_size; }
vec3 voxelToWorld(vec3 p) { return p * g_voxel_world_size - g_half_extents; }

uint brickVoxelMaterial(uint brick_index, ivec3 local) {
	int linear = local.z * 64 + local.y * 8 + local.x;
	int word_idx = linear / 4;
	int byte_lane = linear % 4;
	uint word = terrain.data[8 + g_grid_cells + brick_index * 128 + word_idx];
	return (word >> (byte_lane * 8)) & 0xFFu;
}

uint sampleVoxel(ivec3 brick_cell, ivec3 local) {
	int bs = int(g_brick_size);
	ivec3 actual_cell = brick_cell;
	ivec3 actual_local = local;
	for (int i = 0; i < 3; i++) {
		if (actual_local[i] < 0)        { actual_cell[i] -= 1; actual_local[i] += bs; }
		else if (actual_local[i] >= bs) { actual_cell[i] += 1; actual_local[i] -= bs; }
	}
	if (any(lessThan(actual_cell, ivec3(0))) ||
	    any(greaterThanEqual(actual_cell, ivec3(g_grid_dim)))) return 0u;
	int gx  = int(g_grid_dim.x);
	int gxy = int(g_grid_dim.x * g_grid_dim.y);
	int grid_idx = actual_cell.x + actual_cell.y * gx + actual_cell.z * gxy;
	uint brick_index = terrain.data[8 + grid_idx];
	if (brick_index == 0xFFFFFFFFu) return 0u;
	return brickVoxelMaterial(brick_index, actual_local);
}

bool isSolidAt(ivec3 voxelCoord) {
	if (any(lessThan(voxelCoord, ivec3(0))) ||
	    any(greaterThanEqual(voxelCoord, ivec3(g_volume_size)))) return false;
	int bs = int(g_brick_size);
	ivec3 brick_cell = voxelCoord / bs;
	ivec3 local = voxelCoord - brick_cell * bs;
	int gx  = int(g_grid_dim.x);
	int gxy = int(g_grid_dim.x * g_grid_dim.y);
	int grid_idx = brick_cell.x + brick_cell.y * gx + brick_cell.z * gxy;
	uint brick_index = terrain.data[8 + grid_idx];
	if (brick_index == 0xFFFFFFFFu) return false;
	return brickVoxelMaterial(brick_index, local) != 0u;
}

// Note: shading helpers (sky.glsl / lighting.glsl) are NOT included here
// because they hard-code `pc.skyColor`/`pc.sunColor` etc, and this shader
// holds those fields in the FrameUbo instead. Sky pixels are handled by the
// upstream sky pre-pass (we discard on miss); shadeLit-equivalent math is
// inlined at the bottom of main().

#include "voxel_ao.glsl"

// Substrate shadow query — must come AFTER terrain.data is declared, since
// substrate.glsl includes terrain_brickmap.glsl which references `terrain`
// when SUBSTRATE_TERRAIN is set.
#define SUBSTRATE_TERRAIN
#include "substrate.glsl"

struct Hit {
	bool  hit;
	float t;
	ivec3 brick_cell;
	ivec3 local;
	bvec3 face;
	ivec3 step_sign;
	uint  matIdx;
	int   total_iters;
};

// Two-level DDA — primary rays only. Shadow rays go through traceShadowWorld.
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

	for (int outer_step = 0; outer_step < frame.maxIterations; outer_step++) {
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, grid_dim))) break;

		int grid_idx = cell.x + cell.y * grid_dim.x + cell.z * grid_dim.x * grid_dim.y;
		uint brick_index = terrain.data[8 + grid_idx];

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
	// Mirror the push-constant terrain origin into the named global the
	// terrain_brickmap.glsl helper (and substrate.glsl's terrain check) read.
	terrainOriginVoxel = pc.terrainOriginVoxel;

	// Header — terrain brickmap layout. See src/rendering/voxel/Brickmap.h.
	g_volume_size = uvec3(terrain.data[0], terrain.data[1], terrain.data[2]);
	g_brick_size  = terrain.data[3];
	g_grid_dim    = uvec3(terrain.data[4], terrain.data[5], terrain.data[6]);
	g_grid_cells  = g_grid_dim.x * g_grid_dim.y * g_grid_dim.z;

	g_voxel_world_size = frame.worldVoxelSize;
	g_half_extents     = vec3(g_volume_size) * g_voxel_world_size * 0.5;

	vec3 rayOrigin = frame.cameraPos;
	vec4 transformed = pc.ndcToWorld * vec4(texCoords, 1.0);
	vec3 pixelLocation = transformed.xyz / transformed.w;
	vec3 direction = normalize(pixelLocation - rayOrigin);

	Hit h = trace(rayOrigin, direction);

	if (!h.hit) {
		// Sky already drawn by the sky pre-pass — discard so it shows through.
		discard;
	}

	vec4 albedo = texelFetch(palette_sampler, ivec2(h.matIdx, 0), 0);
	vec3 normal = -vec3(h.step_sign) * vec3(h.face);
	vec3 hitPos = rayOrigin + direction * h.t;

	float ao = 1.0;
	if (frame.aoStrength > 0.0) {
		ivec3 hitVoxel = h.brick_cell * int(g_brick_size) + h.local;
		float raw = cornerAO(hitVoxel, h.face, h.step_sign, hitPos);
		ao = mix(1.0, raw, frame.aoStrength);
	}

	float NdotL = max(0.0, dot(normal, frame.sunDirection));
	float shadow = 1.0;
	if (frame.shadowsEnabled != 0 && NdotL > 0.0) {
		// Receiver's own world voxel — exempt from the substrate's terrain
		// occlusion test so we don't self-shadow at the hit voxel face.
		ivec3 hitWorldVoxel = ivec3(floor(hitPos / g_voxel_world_size));

		// Sub-voxel normal-direction offset to escape numerical precision.
		// Slope term grows at grazing angles where projection foreshortening
		// would otherwise stipple. Same constants the foliage shader uses.
		const float kShadowBiasConstant = 0.002;
		const float kShadowBiasSlope    = 0.05;
		float bias = kShadowBiasConstant
		           + kShadowBiasSlope * (1.0 - clamp(NdotL, 0.0, 1.0));
		vec3 originBiased = hitPos + normal * bias;

		const float kShadowMaxDist = 64.0;
		// CombinedRenderer pins the foliage cloud at world origin (cloud
		// translation = vec3(0)), so cloud-local voxel coords equal world
		// voxel coords. That's why we can pass `vec3(0)` as the cloud
		// origin and `hitWorldVoxel` directly as the skip voxel.
		shadow = traceShadowWorld(originBiased, frame.sunDirection,
		                          kShadowMaxDist, frame.worldVoxelSize,
		                          vec3(0.0), hitWorldVoxel);
	}

	// Inlined equivalent of lighting.glsl::shadeLit, using FrameUbo fields.
	vec3 ambient = frame.skyColor * frame.ambientIntensity * ao;
	vec3 direct  = frame.sunColor * frame.sunIntensity * NdotL * shadow;
	vec4 color = vec4(albedo.rgb * (ambient + direct), 1.0);
	if (frame.debugColor != 0) {
		color -= vec4(vec3(float(h.total_iters) / 250.0), 0.0);
	}
	outColor = color;

	// Write gl_FragDepth at the terrain voxel hit so the foliage pass (which
	// runs after this one with LoadOp::Load on depth) z-tests correctly. The
	// fullscreen quad's vertex output sits at NDC z = 0, so without an
	// explicit FragDepth write everything would render IN FRONT of foliage.
	// Vulkan + GLM_FORCE_DEPTH_ZERO_TO_ONE → clipPos.z/w already in [0, 1].
	vec4 clipPos = frame.viewProj * vec4(hitPos, 1.0);
	gl_FragDepth = clipPos.z / clipPos.w;
}
