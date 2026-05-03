#version 450

// Voxel-volume preview fragment. Companion to voxel_preview.vert.
//
// DDAs from the cube-entry point along the camera→fragment ray. On hit:
// palette-sample → Lambert against the scene sun → ambient-lit by the scene
// sky color → write per-fragment hit depth so the volume integrates correctly
// with the rest of the scene's depth buffer.
//
// Frame addressing: the volume image packs frames as Z-slabs of one 3D
// image. M3 always uses frameIdx=0 (single-frame preview); M4 will animate
// frameIdx against engine time without changing this shader.

layout(push_constant) uniform PC {
    mat4 model;
    vec3 aabbMin;  float _pad0;
    vec3 aabbMax;  float _pad1;
} pc;

// Slot 0 — shared per-frame UBO (matches skinned_mesh.{vert,frag}).
layout(set = 0, binding = 0) uniform FrameUbo {
    mat4  viewProj;
    mat4  ndcToWorld;
    vec3  cameraWorldPos;     int   _pad0;
    vec3  sunDirection;       float sunCosHalfAngle;
    vec3  sunColor;           float sunIntensity;
    vec3  skyColor;           float ambientIntensity;
} frame;

// Slot 1 — voxel-preview-only per-volume meta.
layout(set = 0, binding = 1) uniform DrawUbo {
    vec3  cameraLocalPos;     int frameIdx;
    ivec3 size;               int frameCount;
    int   maxIterations;      int _pad0;  int _pad1;  int _pad2;
} draw;

layout(set = 0, binding = 2) uniform usampler3D volume_sampler;
layout(set = 0, binding = 3) uniform sampler2D  palette_sampler;

layout(location = 0) in  vec3 vLocalPos;
layout(location = 0) out vec4 outColor;

// ---- DDA helper plumbing ----
//
// instanced_voxel_dda.glsl expects three globals to exist in this scope:
//   - `meta` : { ivec3 size; int frameCount; }
//   - `vFrameIdx` : int  (which Z-slab to address)
//   - `g_voxel_local`, `g_local_origin`, `uMaxIterations`
//   - sampleMaterial / isSolidAt / localToVoxel functions
//
// The runtime InstancedVoxelTechnique provides those via uniform blocks +
// flat varyings; we provide the same names via a small local struct + globals
// so the include below resolves identically.

struct MetaProxy { ivec3 size; int frameCount; };
MetaProxy meta = MetaProxy(draw.size, draw.frameCount);
int  vFrameIdx;          // assigned in main()
vec3 g_voxel_local;
vec3 g_local_origin;
int  uMaxIterations;

vec3 localToVoxel(vec3 p) { return (p - g_local_origin) / g_voxel_local; }

uint sampleMaterial(ivec3 voxelCoord) {
    if (any(lessThan(voxelCoord, ivec3(0))) ||
        any(greaterThanEqual(voxelCoord, meta.size))) return 0u;
    // Single-frame preview today; vFrameIdx>0 is M4 territory.
    ivec3 c = ivec3(voxelCoord.x, voxelCoord.y, voxelCoord.z + vFrameIdx * meta.size.z);
    return texelFetch(volume_sampler, c, 0).r;
}

bool isSolidAt(ivec3 voxelCoord) { return sampleMaterial(voxelCoord) != 0u; }

#include "instanced_voxel_dda.glsl"

void main() {
    g_local_origin = pc.aabbMin;
    g_voxel_local  = (pc.aabbMax - pc.aabbMin) / vec3(meta.size);
    uMaxIterations = draw.maxIterations;
    vFrameIdx      = draw.frameIdx;

    // Ray in mesh-local AABB space — both endpoints are already in that frame
    // (cameraLocalPos is the inverse-model-projected camera position, computed
    // CPU-side per frame). Tiny step inward to avoid sitting exactly on the
    // face plane the cube was rasterized at.
    vec3 localDir = normalize(vLocalPos - draw.cameraLocalPos);
    vec3 ddaOrigin = vLocalPos + localDir * 0.0001;

    DdaHit h = traceLocal(ddaOrigin, localDir);
    if (!h.hit) discard;

    // Palette lookup. The palette image is a 256×1 RGBA8 texture; index 0
    // is the empty sentinel and traceLocal won't return it.
    vec4 albedo = texelFetch(palette_sampler, ivec2(h.matIdx, 0), 0);

    // Surface normal is the negative of the step direction along the entry
    // axis — same trick the InstancedVoxel shader uses. Push to world space
    // through the model rotation for the lighting term.
    vec3 localNormal = -vec3(h.step_sign) * vec3(h.face);
    vec3 worldNormal = normalize(mat3(pc.model) * localNormal);

    // Lambert with scene sun + sky-color ambient. Same lighting model as
    // skinned_mesh.frag so toggling Mesh/Voxels modes doesn't visually
    // re-light the asset.
    float ndotl = max(dot(worldNormal, frame.sunDirection), 0.0);
    vec3 ambient = frame.skyColor * frame.ambientIntensity;
    vec3 direct  = frame.sunColor * frame.sunIntensity * ndotl;
    vec3 lit     = albedo.rgb * (ambient + direct);

    // Per-fragment hit depth — the cube was just rasterization scaffolding;
    // the actual depth boundary is where the ray first hit a solid voxel.
    vec3 hitLocal = ddaOrigin + localDir * h.entryT;
    vec4 hitClip  = frame.viewProj * pc.model * vec4(hitLocal, 1.0);
    gl_FragDepth  = hitClip.z / hitClip.w;
    outColor = vec4(lit, 1.0);
}
