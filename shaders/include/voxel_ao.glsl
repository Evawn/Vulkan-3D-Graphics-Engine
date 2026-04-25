// Minecraft-style corner ambient occlusion for a voxel face hit.
//
// REQUIRES the includer to define:
//   bool isSolidAt(ivec3 voxelCoord)   // true if voxel at flat coord is non-empty
//   vec3 worldToVoxel(vec3 p)          // world-space -> voxel-space transform

// Vertex AO: 3 solid neighbors -> darkest, 0 -> fully lit.
// side1+side2 both solid forces max darkness regardless of diagonal.
float vertexAO(float s1, float s2, float c) {
	if (s1 + s2 == 2.0) return 0.0;
	return 1.0 - (s1 + s2 + c) / 3.0;
}

// Bilinear corner AO over the hit face. Samples three neighbors per corner in
// the +N half-space (the empty side the ray came from) and interpolates using
// the hit's fractional position on the face.
float cornerAO(ivec3 hitVoxel, bvec3 face, ivec3 step_sign, vec3 hitPos) {
	// Outward normal (unit integer) = -step_sign on the face axis.
	ivec3 N = -step_sign * ivec3(face);

	// Tangent axes — pick the two whose face component is false.
	ivec3 t1, t2;
	if (face.x)      { t1 = ivec3(0,1,0); t2 = ivec3(0,0,1); }
	else if (face.y) { t1 = ivec3(1,0,0); t2 = ivec3(0,0,1); }
	else             { t1 = ivec3(1,0,0); t2 = ivec3(0,1,0); }

	// Fractional position on the face in [0,1] along each tangent.
	vec3 p_v = worldToVoxel(hitPos);
	vec3 inVoxel = p_v - vec3(hitVoxel);
	float fb = clamp(dot(inVoxel, vec3(t1)), 0.0, 1.0);
	float fc = clamp(dot(inVoxel, vec3(t2)), 0.0, 1.0);

	float v[4];
	for (int s1 = 0; s1 <= 1; s1++) {
		for (int s2 = 0; s2 <= 1; s2++) {
			ivec3 dt1 = (s1 == 0 ? -t1 : t1);
			ivec3 dt2 = (s2 == 0 ? -t2 : t2);
			float side1 = isSolidAt(hitVoxel + N + dt1)       ? 1.0 : 0.0;
			float side2 = isSolidAt(hitVoxel + N + dt2)       ? 1.0 : 0.0;
			float diag  = isSolidAt(hitVoxel + N + dt1 + dt2) ? 1.0 : 0.0;
			v[s1 * 2 + s2] = vertexAO(side1, side2, diag);
		}
	}

	// Bilinear: v[0]=low-low, v[1]=low-high, v[2]=high-low, v[3]=high-high.
	float ao_lowB  = mix(v[0], v[1], fc);
	float ao_highB = mix(v[2], v[3], fc);
	return mix(ao_lowB, ao_highB, fb);
}
