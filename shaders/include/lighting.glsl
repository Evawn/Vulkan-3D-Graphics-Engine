// Surface shading: combine albedo with ambient (sky-tinted, AO-modulated) +
// direct (sun) terms. Caller supplies NdotL and a shadow factor (1=lit, 0=shadowed).
//
// REQUIRES the includer to define a push-constant block named `pc` with at least:
//   vec3  skyColor          // also used as ambient tint
//   float ambientIntensity
//   vec3  sunColor
//   float sunIntensity

vec3 shadeLit(vec3 albedo, float ao, float NdotL, float shadow) {
	vec3 ambient = pc.skyColor * pc.ambientIntensity * ao;
	vec3 direct  = pc.sunColor * pc.sunIntensity * NdotL * shadow;
	return albedo * (ambient + direct);
}
