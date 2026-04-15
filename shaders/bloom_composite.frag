#version 450

// Additive composite: output = scene + bloom * intensity. Intentionally kept at
// LDR — bright pixels will clip at 1.0 once written to the B8G8R8A8_UNORM
// target, which is fine for v1.
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uBloom;

layout(push_constant) uniform PC {
	float intensity;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
	vec3 scene = texture(uScene, vUV).rgb;
	vec3 bloom = texture(uBloom, vUV).rgb;
	outColor = vec4(scene + bloom * pc.intensity, 1.0);
}
