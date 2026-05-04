#version 450

// SkinnedMesh vertex pipeline. Layout matches gltf_import::SkinnedVertex:
//   position (vec3) + normal (vec3) + uv (vec2) + joints (uvec4) + weights (vec4)
// Joints index into a global joint matrix SSBO at offset `firstJoint` (push
// constant). Each draw can therefore reference its own contiguous range of
// joints inside the per-frame arena that the technique uploads in one shot.
//
// Per-frame state (camera + sky/sun) is shared with the voxel preview pass
// via a single GltfImportFrameUbo bound at slot 0 — see GltfImportTechnique.cpp.
//
// M6: push constant grew by 16 bytes for alphaCutoff + alphaMode + pad. The
// fragment shader is the consumer of those fields; the vertex shader passes
// `baseColorFactor.rgb` and `baseColorFactor.a` through to the fragment via
// vBaseColor + vBaseAlpha.

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
    float aoStrength;         int   shadowsEnabled;
    int   _pad1;              int   _pad2;
} frame;

// SSBO over all joints emitted this frame. firstJoint points at the start of
// the run for this draw; the shader reads `joints[firstJoint + idx]`.
layout(set = 0, binding = 1) readonly buffer JointMatrices {
    mat4 joints[];
} jointBuf;

// Push constant — 112 bytes total. Layout matches `SkinnedMeshPC` in
// GltfImportTechnique.cpp; 8 bytes of trailing pad keep std140 alignment.
layout(push_constant) uniform PC {
    mat4  model;            // 64
    vec4  baseColorFactor;  // 16
    uint  firstJoint;       //  4
    uint  jointCount;       //  4
    float alphaCutoff;      //  4
    uint  alphaMode;        //  4    0=Opaque, 1=Mask, 2=Blend
    float meshAlpha;        //  4    Overlay-mode per-pass alpha; 1 otherwise
    uint  _pad1;            //  4
    uint  _pad2;            //  4
    uint  _pad3;            //  4
} pc;

layout(location = 0) out vec3  vWorldNormal;
layout(location = 1) out vec3  vBaseColor;
layout(location = 2) out vec2  vUV;
layout(location = 3) out float vBaseAlpha;

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

    vBaseColor  = pc.baseColorFactor.rgb;
    vBaseAlpha  = pc.baseColorFactor.a;
    vUV         = inUV;
}
