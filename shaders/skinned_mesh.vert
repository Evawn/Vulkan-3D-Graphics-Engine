#version 450

// SkinnedMesh vertex pipeline. Layout matches gltf_import::SkinnedVertex:
//   position (vec3) + normal (vec3) + uv (vec2) + joints (uvec4) + weights (vec4)
// Joints index into a global joint matrix SSBO at offset `firstJoint` (push
// constant). Each draw can therefore reference its own contiguous range of
// joints inside the per-frame arena that the technique uploads in one shot.
//
// Per-frame state (camera + sky/sun) is shared with the voxel preview pass
// via a single GltfImportFrameUbo bound at slot 0 — see GltfImportTechnique.cpp.

layout(location = 0) in vec3  inPosition;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4  inWeights;

layout(set = 0, binding = 0) uniform FrameUbo {
    mat4  viewProj;
    mat4  ndcToWorld;
    vec3  cameraWorldPos;     int   _pad0;
    vec3  sunDirection;       float sunCosHalfAngle;
    vec3  sunColor;           float sunIntensity;
    vec3  skyColor;           float ambientIntensity;
} frame;

// SSBO over all joints emitted this frame. firstJoint points at the start of
// the run for this draw; the shader reads `joints[firstJoint + idx]`.
layout(set = 0, binding = 1) readonly buffer JointMatrices {
    mat4 joints[];
} jointBuf;

layout(push_constant) uniform PC {
    mat4  model;
    vec4  baseColorFactor;
    uint  firstJoint;
    uint  jointCount;
    uint  _pad0;
    uint  _pad1;
} pc;

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec3 vBaseColor;
layout(location = 2) out vec2 vUV;

void main() {
    // Build the linear blend skinning matrix: weighted sum of the four joint
    // matrices addressed by inJoints. firstJoint offsets into the global SSBO
    // arena so multiple skinned draws can share one upload.
    mat4 skin =
          inWeights.x * jointBuf.joints[pc.firstJoint + inJoints.x]
        + inWeights.y * jointBuf.joints[pc.firstJoint + inJoints.y]
        + inWeights.z * jointBuf.joints[pc.firstJoint + inJoints.z]
        + inWeights.w * jointBuf.joints[pc.firstJoint + inJoints.w];

    vec4 skinnedPos = skin * vec4(inPosition, 1.0);
    vec4 worldPos   = pc.model * skinnedPos;
    gl_Position     = frame.viewProj * worldPos;

    // Normals via the inverse-transpose of the upper 3x3 of (model * skin).
    // glTF rigs are non-uniform-scale-light in practice, so the simpler
    // mat3(model * skin) is acceptable for v1; revisit if shading shows skew.
    mat3 nrm = mat3(pc.model) * mat3(skin);
    vWorldNormal = normalize(nrm * inNormal);

    vBaseColor = pc.baseColorFactor.rgb;
    vUV        = inUV;
}
