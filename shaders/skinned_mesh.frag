#version 450

// Companion to skinned_mesh.vert. M6 wires the per-primitive base-color
// texture (set 1, binding 0) for full-fidelity sampling. The shader does
// `sample × factor`, runs the alpha-test for Mask/Blend modes, and
// shades with the scene sun + sky carried via the shared FrameUbo.

layout(location = 0) in vec3  vWorldNormal;
layout(location = 1) in vec3  vBaseColor;     // RGB factor, raw glTF
layout(location = 2) in vec2  vUV;
layout(location = 3) in float vBaseAlpha;     // factor.a

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

// Per-primitive material set. Bound at descriptor set index 1 by the
// technique's per-draw `vkCmdBindDescriptorSets` call. The combined image
// sampler is the asset's per-primitive baseColorTexture (or the asset's
// 1×1 white fallback for primitives without one — sample × factor then
// passes the factor through as the flat color).
layout(set = 1, binding = 0) uniform sampler2D baseColorTex;

// Same push-constant layout as the vertex shader; only the alpha-related
// fields are read here. `baseColorFactor` is duplicated through
// vBaseColor / vBaseAlpha varyings so we don't need to re-read it per
// pixel — but `alphaCutoff` and `alphaMode` are uniform per draw, so we
// read them straight from the push constant.
layout(push_constant) uniform PC {
    mat4  model;
    vec4  baseColorFactor;
    uint  firstJoint;
    uint  jointCount;
    float alphaCutoff;
    uint  alphaMode;          // 0=Opaque, 1=Mask, 2=Blend
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
    uint  _pad3;
} pc;

void main() {
    // Sample × factor per spec. The texture's sRGB-encoded RGB is
    // automatically linearized by Vulkan when sampled at format
    // VK_FORMAT_R8G8B8A8_SRGB; we operate in linear space from here on.
    vec4 sampled = texture(baseColorTex, vUV);
    vec4 c = sampled * vec4(vBaseColor, vBaseAlpha);

    // Alpha-test for Mask + Blend (Blend treated as Mask in v1; true
    // alpha-blend with depth-sorted draw order is future work). Opaque
    // skips the test — the asset asserts solidity, so honoring sampled
    // alpha would corrupt opaque assets that happen to have alpha != 1
    // in their texture's alpha channel.
    if (pc.alphaMode != 0u && c.a < pc.alphaCutoff) {
        discard;
    }

    vec3 N      = normalize(vWorldNormal);
    float ndotl = max(dot(N, frame.sunDirection), 0.0);
    vec3 ambient = frame.skyColor * frame.ambientIntensity;
    vec3 direct  = frame.sunColor * frame.sunIntensity * ndotl;
    vec3 lit     = c.rgb * (ambient + direct);

    outColor = vec4(lit, pc.alphaMode == 0u ? 1.0 : c.a);
}
