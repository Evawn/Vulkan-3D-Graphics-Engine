#version 450

// Voxel-volume preview fragment. Companion to voxel_preview.vert.
//
// DDAs from the cube-entry point along the camera→fragment ray. On hit:
// palette-sample → Lambert against the scene sun → corner AO from the volume's
// own occupancy → secondary DDA shadow ray toward the sun → ambient-lit by
// the scene sky color → write per-fragment hit depth so the volume integrates
// correctly with the rest of the scene's depth buffer.
//
// Direct lighting (NdotL × sun × shadow) and AO match the model used by
// instanced_voxel.frag and brickmap_palette_trace.frag, so toggling Mesh ↔
// Voxels modes in the import workspace doesn't visually re-light the asset
// and bakes shade the same way they will once promoted into a Scene.
//
// The shadow ray is traced in mesh-local space (the same frame the primary
// DDA marches), so we rotate the world-space sun direction into local space
// once at hit time.
//
// Frame addressing: the volume image packs frames as 2D-array layers (one
// frame per layer). Within a layer voxel (x, y, z) lives at
// (x, y + z*size.y, frameIdx). M3 always uses frameIdx=0 (single-frame
// preview); M4 animates frameIdx against engine time without changing this
// shader.

layout(push_constant) uniform PC {
    mat4 model;
    vec3 aabbMin;  float _pad0;
    // The .w of aabbMax carries the per-pass alpha for the Overlay
    // crossfade. 1.0 in Voxels-only mode (alpha-blend degrades to a pure
    // overwrite); m_overlayBlend in Overlay.
    vec3 aabbMax;  float voxelAlpha;
} pc;

// Slot 0 — shared per-frame UBO (matches skinned_mesh.{vert,frag}).
layout(set = 0, binding = 0) uniform FrameUbo {
    mat4  viewProj;
    mat4  ndcToWorld;
    vec3  cameraWorldPos;     int   _pad0;
    vec3  sunDirection;       float sunCosHalfAngle;
    vec3  sunColor;           float sunIntensity;
    vec3  skyColor;           float ambientIntensity;
    float aoStrength;         int   shadowsEnabled;
    int   _pad1;              int   _pad2;
} frame;

// Slot 1 — voxel-preview-only per-volume meta.
layout(set = 0, binding = 1) uniform DrawUbo {
    vec3  cameraLocalPos;     int frameIdx;
    ivec3 size;               int frameCount;
    int   maxIterations;      int _pad0;  int _pad1;  int _pad2;
} draw;

layout(set = 0, binding = 2) uniform usampler2DArray volume_sampler;
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

// voxel_ao.glsl is shared with the world-space shaders and asks the includer
// for `worldToVoxel`. The preview's "world" is mesh-local AABB space, so
// alias it.
vec3 worldToVoxel(vec3 p) { return localToVoxel(p); }

uint sampleMaterial(ivec3 voxelCoord) {
    if (any(lessThan(voxelCoord, ivec3(0))) ||
        any(greaterThanEqual(voxelCoord, meta.size))) return 0u;
    // Frame-as-layer: layer = vFrameIdx; (y, z) flatten into the layer's
    // 2D extent as y' = y + z*size.y.
    ivec3 c = ivec3(voxelCoord.x, voxelCoord.y + voxelCoord.z * meta.size.y, vFrameIdx);
    return texelFetch(volume_sampler, c, 0).r;
}

bool isSolidAt(ivec3 voxelCoord) { return sampleMaterial(voxelCoord) != 0u; }

#include "voxel_ao.glsl"
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

    // Continuous hit point on the entry face (not the voxel center). Both AO
    // and the shadow ray want this — AO uses it for bilinear interpolation
    // across the face, the shadow ray uses it as the origin so silhouettes
    // project diagonally instead of snapping to the receiver-voxel boundary.
    // Same approach as instanced_voxel.frag and brickmap_palette_trace.frag.
    vec3 hitLocal = ddaOrigin + localDir * h.entryT;

    // Corner AO sampled against the volume's own occupancy — the same
    // `isSolidAt` the primary trace already uses. `worldToVoxel` is aliased
    // to `localToVoxel` above so the helper resolves against mesh-local
    // coords.
    float ao = 1.0;
    if (frame.aoStrength > 0.0) {
        float raw = cornerAO(h.voxel, h.face, h.step_sign, hitLocal);
        ao = mix(1.0, raw, frame.aoStrength);
    }

    // Self-shadow ray: re-use traceLocal in the same mesh-local frame the
    // primary ray marched. Rotate the world-space sun direction into local
    // space — `mat3(pc.model)` carries the SceneNode's rotation+scale, so its
    // transpose serves as the inverse rotation for a unit direction vector.
    // (Per-axis non-uniform scale would warp this slightly; in practice the
    // import pipeline produces uniform-scale model matrices.) NdotL stays in
    // world space so it agrees with the skinned-mesh lighting.
    float ndotl  = max(dot(worldNormal, frame.sunDirection), 0.0);
    float shadow = 1.0;
    if (frame.shadowsEnabled != 0 && ndotl > 0.0) {
        vec3 localSunDir = normalize(transpose(mat3(pc.model)) * frame.sunDirection);
        // Bias along the surface normal by a small fraction of a voxel to
        // avoid the shadow ray re-hitting the receiver's own face.
        float bias = 0.01 * length(g_voxel_local);
        vec3 shadowOrigin = hitLocal + localNormal * bias;
        DdaHit sh = traceLocal(shadowOrigin, localSunDir);
        shadow = sh.hit ? 0.0 : 1.0;
    }

    // Same shading equation as instanced_voxel.frag / brickmap_palette_trace.frag:
    // ambient is sky-tinted and AO-modulated, direct is sun-tinted and
    // shadow-gated. Caller-side aoStrength + shadowsEnabled toggles let the
    // panel disable either term without rebuilding the pipeline.
    vec3 ambient = frame.skyColor * frame.ambientIntensity * ao;
    vec3 direct  = frame.sunColor * frame.sunIntensity * ndotl * shadow;
    vec3 lit     = albedo.rgb * (ambient + direct);

    // Per-fragment hit depth — the cube was just rasterization scaffolding;
    // the actual depth boundary is where the ray first hit a solid voxel.
    // (In Overlay mode the pipeline disables depth-test/write so this is a
    // no-op there; harmless to compute and write either way.)
    vec4 hitClip  = frame.viewProj * pc.model * vec4(hitLocal, 1.0);
    gl_FragDepth  = hitClip.z / hitClip.w;

    // Output alpha drives the Overlay crossfade. In Voxels-only mode
    // pc.voxelAlpha == 1.0 and the alpha-blend math degrades to a pure
    // overwrite, identical to the legacy opaque path.
    outColor = vec4(lit, pc.voxelAlpha);
}
