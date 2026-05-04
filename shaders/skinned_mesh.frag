#version 450

// Companion to skinned_mesh.vert. Lighting now consumes the scene's sun + sky
// state via the shared GltfImportFrameUbo so the mesh and the voxel preview
// stay visually consistent (both lit by the same scene sun, both ambient-lit
// by the same sky color). Texture sampling lands in M5; until then we tint
// with the material's base color factor.

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vBaseColor;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

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

void main() {
    vec3 N = normalize(vWorldNormal);
    float ndotl = max(dot(N, frame.sunDirection), 0.0);
    vec3 ambient = frame.skyColor * frame.ambientIntensity;
    vec3 direct  = frame.sunColor * frame.sunIntensity * ndotl;
    vec3 lit     = vBaseColor * (ambient + direct);
    outColor = vec4(lit, 1.0);
}
