#version 450

// Separable 9-tap Gaussian. Direction set by host (horizontal=(1,0), vertical=(0,1))
// in texel-space; combined with texelSize to walk exactly one pixel per step.
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uSrc;

layout(push_constant) uniform PC {
	vec2 direction;   // (1,0) for horizontal, (0,1) for vertical — in pixel units
	vec2 texelSize;   // 1.0 / textureSize
	float radius;     // scale factor applied to tap offsets (1.0 = native spacing)
} pc;

layout(location = 0) out vec4 outColor;

// Precomputed weights for a σ≈2 Gaussian, symmetric around center.
const float W0 = 0.227027;
const float W1 = 0.194594;
const float W2 = 0.121622;
const float W3 = 0.054054;
const float W4 = 0.016216;

void main() {
	vec2 off = pc.direction * pc.texelSize * pc.radius;
	vec3 sum = texture(uSrc, vUV).rgb * W0;
	sum += texture(uSrc, vUV + off * 1.0).rgb * W1;
	sum += texture(uSrc, vUV - off * 1.0).rgb * W1;
	sum += texture(uSrc, vUV + off * 2.0).rgb * W2;
	sum += texture(uSrc, vUV - off * 2.0).rgb * W2;
	sum += texture(uSrc, vUV + off * 3.0).rgb * W3;
	sum += texture(uSrc, vUV - off * 3.0).rgb * W3;
	sum += texture(uSrc, vUV + off * 4.0).rgb * W4;
	sum += texture(uSrc, vUV - off * 4.0).rgb * W4;
	outColor = vec4(sum, 1.0);
}
