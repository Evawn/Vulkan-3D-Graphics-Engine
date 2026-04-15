#version 450

// Fullscreen vertex shader paired with PipelineDefaults::FullscreenQuad (4-vert
// triangle strip, NoVertexInput). Emits UV in [0,1] for post-process fragment
// shaders to sample input textures.
layout(location = 0) out vec2 vUV;

void main() {
	float x = float((gl_VertexIndex & 1) * 2 - 1);
	float y = float(((gl_VertexIndex & 2) >> 1) * 2 - 1);
	vUV = vec2(x * 0.5 + 0.5, y * 0.5 + 0.5);
	gl_Position = vec4(x, y, 0.0, 1.0);
}
