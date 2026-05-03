#version 450

// Sky pre-pass for the GLB Import workspace. Painted FIRST each frame so the
// skinned-mesh + voxel passes can use LoadOp::Load on color and overlay their
// draws on top of the gradient + sun disk.
//
// Pair with sky_fullscreen.vert (NDC strip — emits vNDC per-pixel).
// Consumes the same GltfImportFrameUbo the rest of the technique's pipelines
// see, so inspector edits to sun direction / sun color / sky color reflect
// here without any extra plumbing.

layout(location = 0) in vec3 vNDC;
layout(location = 0) out vec4 outColor;

// Named `pc` so sky.glsl's missColor() — which references `pc.skyColor`,
// `pc.sunDirection`, `pc.sunCosHalfAngle`, `pc.sunColor`, `pc.sunIntensity`
// — resolves against this block. (sky.glsl is shared with the brickmap +
// instanced-voxel sky shaders, both of which alias their own UBO/push-
// constant to the same name. Don't rename.)
layout(set = 0, binding = 0) uniform FrameUbo {
    mat4  viewProj;
    mat4  ndcToWorld;
    vec3  cameraWorldPos;     int   _pad0;
    vec3  sunDirection;       float sunCosHalfAngle;
    vec3  sunColor;           float sunIntensity;
    vec3  skyColor;           float ambientIntensity;
} pc;

#include "sky.glsl"

void main() {
    // Reproject NDC → world to get the per-pixel ray direction.
    vec4 worldH = pc.ndcToWorld * vec4(vNDC, 1.0);
    vec3 worldPoint = worldH.xyz / worldH.w;
    vec3 direction = normalize(worldPoint - pc.cameraWorldPos);
    outColor = missColor(direction);
}
