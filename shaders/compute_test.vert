#version 450

layout(location = 0) out vec2 fragUV;

void main() {
    float x = float((gl_VertexIndex & 1) * 2 - 1);
    float y = float(((gl_VertexIndex & 2) >> 1) * 2 - 1);
    fragUV = vec2(x * 0.5 + 0.5, y * 0.5 + 0.5);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
