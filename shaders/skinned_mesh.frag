#version 450

// Companion to skinned_mesh.vert. v1 fragment is intentionally minimal:
// directional Lambert with a fixed light + fixed ambient, tinted by the
// material's base color factor. Texture sampling lands in a follow-up
// milestone (M5 in animated-voxel-import.md).

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vBaseColor;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vWorldNormal);
    // A fixed light coming from above and slightly to the side. Good enough
    // to give shape to a previewed mesh; the user gets a real lighting story
    // when the bake feeds CombinedRenderer.
    vec3 L = normalize(vec3(0.3, 1.0, 0.2));
    float ndotl = max(dot(N, L), 0.0);

    vec3 ambient = vec3(0.25);
    vec3 lit     = vBaseColor * (ambient + (1.0 - ambient) * ndotl);
    outColor = vec4(lit, 1.0);
}
