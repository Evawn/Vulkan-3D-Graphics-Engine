#version 450

layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(push_constant) uniform PC {
	float threshold;  // luminance below this is fully rejected
	float knee;       // soft-knee width; smoothstep from threshold..threshold+knee
} pc;

layout(location = 0) out vec4 outColor;

void main() {
	vec3 c = texture(uScene, vUV).rgb;
	// Rec. 709 luma — close enough to perceived brightness for a bright-pass.
	float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
	float w = smoothstep(pc.threshold, pc.threshold + max(pc.knee, 1e-4), lum);
	outColor = vec4(c * w, 1.0);
}
