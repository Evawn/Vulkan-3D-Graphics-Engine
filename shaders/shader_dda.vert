#version 450

layout(location =0) out vec3 texCoords;

void main() {
        float x = float((gl_VertexIndex & 1) * 2 - 1);
    float y = float(((gl_VertexIndex & 2) >> 1) * 2 - 1);
    texCoords = vec3(x, y, 0.0);
    gl_Position = vec4(x, y, 0.0, 1.0);
}