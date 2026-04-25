// Sky + sun-disk rendering for non-hit rays.
//
// REQUIRES the includer to define a push-constant block named `pc` with at least:
//   vec3  skyColor
//   vec3  sunDirection      // normalized, world-space (from origin toward sun)
//   float sunCosHalfAngle   // cos(half apparent-size)
//   vec3  sunColor          // disk tint
//   float sunIntensity      // disk brightness scalar

const float SKY_PI_OVER_2 = asin(1.0);
const vec4  SKY_HORIZON_COLOR = vec4(0.8, 0.9, 1.0, 1.0);

vec4 missColor(vec3 direction) {
	float dotProd = clamp(dot(direction, vec3(0.0, 0.0, 1.0)), -1.0, 1.0);
	float theta = acos(dotProd) / SKY_PI_OVER_2;
	vec4 sky = vec4(pc.skyColor, 1.0);
	vec4 base;
	if (theta < 1.0) base = sky * (1.0 - theta) + SKY_HORIZON_COLOR * theta;
	else base = SKY_HORIZON_COLOR * (2.0 - theta);

	// Sun disk: soft-edge mask over the sky. Mask edge width is a small fraction
	// of the cosine space so the disk edge matches the apparent angular size
	// regardless of how large the sun is configured.
	float sunDot = dot(direction, pc.sunDirection);
	float edge = max(1e-4, (1.0 - pc.sunCosHalfAngle) * 0.25);
	float mask = smoothstep(pc.sunCosHalfAngle - edge, pc.sunCosHalfAngle + edge, sunDot);
	vec3 disk = pc.sunColor * pc.sunIntensity;
	return vec4(mix(base.rgb, disk, mask), 1.0);
}
