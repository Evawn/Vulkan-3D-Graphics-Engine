#version 450

// Voxel-volume preview vertex pipeline. Companion to voxel_preview.frag.
//
// Rasterizes a 36-vertex unit cube scaled to the bake's mesh-local AABB and
// transformed by the SceneNode's world matrix. Same procedural cube layout
// the InstancedVoxelTechnique uses (see instanced_voxel.vert) — minus the
// per-instance SSBO, because the GLB import preview only ever has a single
// volume.
//
// vLocalPos is the cube vertex in mesh-local AABB coords (aabbMin..aabbMax).
// The DDA helper in the fragment traces in this same frame, so we don't need
// to undo the model matrix per ray step.

layout(push_constant) uniform PC {
    mat4 model;          // SceneNode world transform
    vec3 aabbMin;  float _pad0;
    // The .w of aabbMax holds the per-pass alpha for the Overlay
    // crossfade. The vertex pipeline doesn't need it; the fragment does.
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

layout(location = 0) out vec3 vLocalPos;

// 36-vertex unit cube (positions in [0,1]^3). Two triangles per face × 6 faces.
const vec3 kCube[36] = vec3[36](
    // -X
    vec3(0,0,0), vec3(0,1,1), vec3(0,1,0),
    vec3(0,0,0), vec3(0,0,1), vec3(0,1,1),
    // +X
    vec3(1,0,0), vec3(1,1,0), vec3(1,1,1),
    vec3(1,0,0), vec3(1,1,1), vec3(1,0,1),
    // -Y
    vec3(0,0,0), vec3(1,0,0), vec3(1,0,1),
    vec3(0,0,0), vec3(1,0,1), vec3(0,0,1),
    // +Y
    vec3(0,1,0), vec3(1,1,1), vec3(1,1,0),
    vec3(0,1,0), vec3(0,1,1), vec3(1,1,1),
    // -Z
    vec3(0,0,0), vec3(1,1,0), vec3(1,0,0),
    vec3(0,0,0), vec3(0,1,0), vec3(1,1,0),
    // +Z
    vec3(0,0,1), vec3(1,0,1), vec3(1,1,1),
    vec3(0,0,1), vec3(1,1,1), vec3(0,1,1)
);

void main() {
    vec3 unit = kCube[gl_VertexIndex];
    vec3 localPos = pc.aabbMin + unit * (pc.aabbMax - pc.aabbMin);
    vec4 worldPos = pc.model * vec4(localPos, 1.0);
    gl_Position = frame.viewProj * worldPos;
    vLocalPos = localPos;
}
