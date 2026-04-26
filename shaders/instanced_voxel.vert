#version 450

// Instanced voxel mesh — bounding-box rasterization.
//
// Each instance rasterizes a unit cube whose corners are picked from a
// hardcoded 36-vertex (12-tri) table indexed by gl_VertexIndex. The cube is
// scaled to (instanceAabbMax - instanceAabbMin) and transformed by the
// per-instance TRS in the SSBO + the cloud-level world transform from the
// push constant. The fragment shader DDAs *inside* this cube against the
// shared voxel asset, animated by per-instance frameOffset.

layout(push_constant) uniform PushConstantBlock {
	mat4  cloudWorld;       // per-cloud world transform (from SceneNode hierarchy)
	mat4  viewProj;         // camera view * projection
	vec3  cameraPos;        // world-space camera, for fragment ray origin
	int   maxIterations;
	vec3  skyColor;
	int   debugColor;
	vec3  sunDirection;
	float sunCosHalfAngle;
	vec3  sunColor;
	float sunIntensity;
	vec3  aabbMin;          // per-instance AABB extent (instance-local)
	float ambientIntensity;
	vec3  aabbMax;
	float aoStrength;
	int   shadowsEnabled;
	float time;             // seconds since start; per-instance frame index = time + animOffset
	int   frameCount;
	int   _pad0;
} pc;

struct InstanceData {
	vec3  position;       float scale;
	vec4  rotation;       // quaternion (xyz, w)
	float animOffset;     // seconds added to time before mod-by-frameCount
	float _pad0; float _pad1; float _pad2;
};

layout(set = 0, binding = 0) readonly buffer InstanceBuffer {
	InstanceData instances[];
} ib;

layout(location = 0) out vec3 vLocalPos;     // position inside the per-instance AABB (for DDA entry)
layout(location = 1) out vec3 vWorldPos;     // for ray-origin in fragment if needed
layout(location = 2) out flat int vFrameIdx; // frame slab to sample

// 36-vertex unit cube (positions in [0,1]^3). Two triangles per face × 6 faces.
// Order: -X, +X, -Y, +Y, -Z, +Z. Winding chosen so the front face is the
// outward-pointing one (back-face culling can be on with CCW front-face).
const vec3 kCube[36] = vec3[36](
	// -X
	vec3(0,0,0), vec3(0,1,1), vec3(0,1,0),
	vec3(0,0,0), vec3(0,0,1), vec3(0,1,1),
	// +X
	vec3(1,0,0), vec3(1,1,0), vec3(1,1,1),
	vec3(1,0,0), vec3(1,1,1), vec3(1,0,1),
	// -Y
	vec3(0,0,0), vec3(1,0,0), vec3(1,0,1),
	vec3(0,0,0), vec3(1,0,1), vec3(0,0,1),
	// +Y
	vec3(0,1,0), vec3(1,1,1), vec3(1,1,0),
	vec3(0,1,0), vec3(0,1,1), vec3(1,1,1),
	// -Z
	vec3(0,0,0), vec3(1,1,0), vec3(1,0,0),
	vec3(0,0,0), vec3(0,1,0), vec3(1,1,0),
	// +Z
	vec3(0,0,1), vec3(1,0,1), vec3(1,1,1),
	vec3(0,0,1), vec3(1,1,1), vec3(0,1,1)
);

vec3 quatRotate(vec4 q, vec3 v) {
	vec3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

void main() {
	InstanceData inst = ib.instances[gl_InstanceIndex];

	vec3 unit = kCube[gl_VertexIndex];
	vec3 localPos = pc.aabbMin + unit * (pc.aabbMax - pc.aabbMin);

	// Per-instance: scale → rotate → translate.
	vec3 scaled  = localPos * inst.scale;
	vec3 rotated = quatRotate(inst.rotation, scaled);
	vec3 inCloud = rotated + inst.position;

	vec4 world = pc.cloudWorld * vec4(inCloud, 1.0);
	gl_Position = pc.viewProj * world;

	vLocalPos = localPos;
	vWorldPos = world.xyz;
	float t = pc.time + inst.animOffset;
	int fc = max(pc.frameCount, 1);
	int frame = int(floor(mod(t, float(fc))));
	if (frame < 0) frame += fc;
	vFrameIdx = frame;
}
