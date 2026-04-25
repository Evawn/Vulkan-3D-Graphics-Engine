#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct VoxModel {
	uint32_t sizeX, sizeY, sizeZ;                 // raw composited bbox size (not padded)
	glm::uvec3 volumeSize;                        // per-axis volume (each axis padded to multiple of 8)
	std::vector<uint8_t> volume;                  // volumeSize.x * y * z bytes, material indices, 0=empty
	std::array<uint8_t, 256 * 4> palette;         // RGBA palette (index 0 unused)
};

std::optional<VoxModel> LoadVoxFile(const std::string& path);
