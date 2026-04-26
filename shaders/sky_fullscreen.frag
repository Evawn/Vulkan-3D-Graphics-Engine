#version 450

// Fullscreen sky pre-pass. Builds a world-space ray from per-pixel NDC and
// lets `sky.glsl::missColor` paint the gradient + sun disk. Reads the same
// per-frame UBO the trace pass uses, so sky/sun edits in the inspector are
// reflected here without any extra plumbing.

layout(location = 0) in vec3 vNDC;
layout(location = 0) out vec4 outColor;

// `pc` is the name `sky.glsl` expects. Sky frag has no push constant, so we
// alias the frame UBO to the same struct contract instead.
layout(set = 0, binding = 0) uniform FrameUbo {
	mat4  viewProj;          // unused here — the NDC-to-world inverse is what we need
	mat4  ndcToWorld;
	vec3  cameraPos;         int   maxIterations;
	vec3  skyColor;
	int   debugColor;
	vec3  sunDirection;      float sunCosHalfAngle;
	vec3  sunColor;          float sunIntensity;
	float ambientIntensity;
	float aoStrength;
	int   shadowsEnabled;
	float time;
	int   frameCount;
	int   _pad0; int _pad1; int _pad2;
} pc;

#include "sky.glsl"

void main() {
	vec4 worldH = pc.ndcToWorld * vec4(vNDC, 1.0);
	vec3 worldPoint = worldH.xyz / worldH.w;
	vec3 direction = normalize(worldPoint - pc.cameraPos);
	outColor = missColor(direction);
}
