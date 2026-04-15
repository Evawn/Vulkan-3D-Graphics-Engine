#version 450

// Analytic lens flare: reads the bloomed scene and adds screen-space ghosts and a
// halo based on the sun's projected position. The whole effect is pure addition;
// when the sun is behind the camera (sunVisible == 0) the pass is a pass-through.
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(push_constant) uniform PC {
	vec2 sunScreen;    // sun position in UV space [0,1]^2; may be outside for off-screen sun
	float sunVisible;  // 1.0 if the sun projects in front of the camera, 0.0 otherwise
	float intensity;   // global flare multiplier
	float haloRadius;  // in UV units (e.g. 0.25 = quarter-screen halo)
	float ghostSpread; // spacing of ghosts along sun→center axis (UV units along the line)
	float aspect;      // viewport width / height — compensates circle shape in UV space
	float _pad;
} pc;

layout(location = 0) out vec4 outColor;

// Soft disk, returns 1 at center, fades to 0 at `radius` with a quadratic falloff.
float disk(vec2 uv, vec2 center, float radius) {
	vec2 d = (uv - center) * vec2(pc.aspect, 1.0);
	float r = length(d) / max(radius, 1e-4);
	return pow(max(0.0, 1.0 - r), 2.0);
}

void main() {
	vec3 scene = texture(uScene, vUV).rgb;

	if (pc.sunVisible < 0.5) {
		outColor = vec4(scene, 1.0);
		return;
	}

	// Direction from the sun toward the screen center; ghosts march along this line.
	vec2 center = vec2(0.5);
	vec2 axis = center - pc.sunScreen;

	// Fade the whole effect as the sun moves off-screen — the halo shouldn't glow
	// the entire view when the sun is just past the edge.
	vec2 sunDist = abs(pc.sunScreen - center);
	float offScreen = smoothstep(0.7, 1.1, max(sunDist.x, sunDist.y));
	float edgeFade = 1.0 - offScreen;

	// Halo centered on the sun — warm, broad.
	vec3 halo = vec3(1.0, 0.95, 0.85) * disk(vUV, pc.sunScreen, pc.haloRadius) * 0.6;

	// 5 ghosts along the axis with descending size and chromatic tints.
	// Fractions chosen to space them visually between the sun and just past center.
	vec3 ghosts = vec3(0.0);
	float fractions[5] = float[](0.35, 0.55, 0.75, 1.0, 1.35);
	float sizes[5]     = float[](0.045, 0.06, 0.03, 0.08, 0.025);
	vec3 tints[5] = vec3[](
		vec3(1.0, 0.6, 0.4),   // warm
		vec3(0.5, 0.9, 0.7),   // aqua
		vec3(0.8, 0.6, 1.0),   // violet
		vec3(1.0, 0.9, 0.5),   // gold
		vec3(0.6, 0.8, 1.0)    // blue
	);
	for (int i = 0; i < 5; i++) {
		vec2 pos = pc.sunScreen + axis * fractions[i] * pc.ghostSpread;
		ghosts += tints[i] * disk(vUV, pos, sizes[i]);
	}

	vec3 flare = (halo + ghosts) * pc.intensity * edgeFade;
	outColor = vec4(scene + flare, 1.0);
}
