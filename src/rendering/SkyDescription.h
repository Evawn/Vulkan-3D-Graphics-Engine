#pragma once

#include <glm/glm.hpp>

// Scene-wide sky description. Today: a procedural gradient parameterized by a
// single color (the same uniform sky color the brickmap + animated-geometry
// trace shaders consume). Future expansion (separate fields, not a tagged
// union, until we actually need a cubemap):
//   - `AssetID cubemap;`         — sampled by a future Skybox technique
//   - `glm::vec3 horizonColor;`  — gradient bottom
//   - `glm::vec3 zenithColor;`   — gradient top
//
// Decoupled from SceneLighting: sky and lighting share the inspector panel but
// they're conceptually separate (sky is "what you see at infinity", lighting
// is "where photons come from"). The future foliage editor will swap sky
// presets per-environment without touching sun direction.
struct SkyDescription {
	// Procedural sky color — what the trace shaders sample for rays that miss
	// scene geometry. Equivalent to the previous per-technique m_sky_color.
	glm::vec3 color = glm::vec3(0.529f, 0.808f, 0.922f);
};
