#version 450

// Fullscreen sky pre-pass vertex shader. Emits a 4-vertex triangle strip
// covering the entire screen in NDC; the fragment shader uses the per-pixel
// NDC value to build a world-space ray for the sky/sun gradient.

layout(location = 0) out vec3 vNDC;

void main() {
	float x = float((gl_VertexIndex & 1) * 2 - 1);
	float y = float(((gl_VertexIndex & 2) >> 1) * 2 - 1);
	vNDC = vec3(x, y, 0.0);
	gl_Position = vec4(x, y, 0.0, 1.0);
}
