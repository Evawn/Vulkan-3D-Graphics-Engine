#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct VoxModel {
	uint32_t sizeX, sizeY, sizeZ;
	uint32_t volumeSize;                          // cube dimension (multiple of 8)
	std::vector<uint8_t> volume;                  // volumeSize^3 bytes, material indices, 0=empty
	std::array<uint8_t, 256 * 4> palette;         // RGBA palette (index 0 unused)
};

std::optional<VoxModel> LoadVoxFile(const std::string& path);
