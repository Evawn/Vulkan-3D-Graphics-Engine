#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

// Scene-wide lighting state shared between render techniques (used for sky/shading)
// and post-process effects (used for bloom bright-pass + lens flare placement).
// Sun direction points FROM the sun TOWARD the scene, i.e. the direction light travels.
// When tracing into the sky the shader compares a view ray against -sunDirection.
struct SceneLighting {
	// Spherical controls (degrees) edited by ImGui — direction derived from these.
	float sunAzimuth = 45.0f;     // around Z, 0=+X axis
	float sunElevation = 30.0f;   // up from XY plane toward +Z
	float sunAngularSize = 1.5f;  // apparent diameter in degrees (real sun ≈ 0.53°)
	float sunIntensity = 1.0f;    // scalar multiplier for disk color
	float sunColor[3] = { 1.0f, 0.98f, 0.92f };

	// Direction pointing from origin toward the sun (unit vector). World-space, +Z = up.
	glm::vec3 GetSunDirection() const {
		float az = glm::radians(sunAzimuth);
		float el = glm::radians(sunElevation);
		float ce = std::cos(el);
		return glm::normalize(glm::vec3(ce * std::cos(az), ce * std::sin(az), std::sin(el)));
	}

	// Half-angle cosine used by the sky shader to test "is this ray inside the sun disk".
	float GetSunCosHalfAngle() const {
		return std::cos(glm::radians(sunAngularSize * 0.5f));
	}
};
