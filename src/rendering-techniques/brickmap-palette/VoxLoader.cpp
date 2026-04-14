#include "VoxLoader.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <fstream>
#include <cstring>

// MagicaVoxel default palette (used when no RGBA chunk is present).
// 256 entries as 0xAARRGGBB, converted to RGBA bytes below.
static constexpr uint32_t kDefaultPaletteABGR[256] = {
	0x00000000, 0xffffffff, 0xffccffff, 0xff99ffff, 0xff66ffff, 0xff33ffff, 0xff00ffff, 0xffffccff,
	0xffccccff, 0xff99ccff, 0xff66ccff, 0xff33ccff, 0xff00ccff, 0xffff99ff, 0xffcc99ff, 0xff9999ff,
	0xff6699ff, 0xff3399ff, 0xff0099ff, 0xffff66ff, 0xffcc66ff, 0xff9966ff, 0xff6666ff, 0xff3366ff,
	0xff0066ff, 0xffff33ff, 0xffcc33ff, 0xff9933ff, 0xff6633ff, 0xff3333ff, 0xff0033ff, 0xffff00ff,
	0xffcc00ff, 0xff9900ff, 0xff6600ff, 0xff3300ff, 0xff0000ff, 0xffffffcc, 0xffccffcc, 0xff99ffcc,
	0xff66ffcc, 0xff33ffcc, 0xff00ffcc, 0xffffcccc, 0xffcccccc, 0xff99cccc, 0xff66cccc, 0xff33cccc,
	0xff00cccc, 0xffff99cc, 0xffcc99cc, 0xff9999cc, 0xff6699cc, 0xff3399cc, 0xff0099cc, 0xffff66cc,
	0xffcc66cc, 0xff9966cc, 0xff6666cc, 0xff3366cc, 0xff0066cc, 0xffff33cc, 0xffcc33cc, 0xff9933cc,
	0xff6633cc, 0xff3333cc, 0xff0033cc, 0xffff00cc, 0xffcc00cc, 0xff9900cc, 0xff6600cc, 0xff3300cc,
	0xff0000cc, 0xffffff99, 0xffccff99, 0xff99ff99, 0xff66ff99, 0xff33ff99, 0xff00ff99, 0xffffcc99,
	0xffcccc99, 0xff99cc99, 0xff66cc99, 0xff33cc99, 0xff00cc99, 0xffff9999, 0xffcc9999, 0xff999999,
	0xff669999, 0xff339999, 0xff009999, 0xffff6699, 0xffcc6699, 0xff996699, 0xff666699, 0xff336699,
	0xff006699, 0xffff3399, 0xffcc3399, 0xff993399, 0xff663399, 0xff333399, 0xff003399, 0xffff0099,
	0xffcc0099, 0xff990099, 0xff660099, 0xff330099, 0xff000099, 0xffffff66, 0xffccff66, 0xff99ff66,
	0xff66ff66, 0xff33ff66, 0xff00ff66, 0xffffcc66, 0xffcccc66, 0xff99cc66, 0xff66cc66, 0xff33cc66,
	0xff00cc66, 0xffff9966, 0xffcc9966, 0xff999966, 0xff669966, 0xff339966, 0xff009966, 0xffff6666,
	0xffcc6666, 0xff996666, 0xff666666, 0xff336666, 0xff006666, 0xffff3366, 0xffcc3366, 0xff993366,
	0xff663366, 0xff333366, 0xff003366, 0xffff0066, 0xffcc0066, 0xff990066, 0xff660066, 0xff330066,
	0xff000066, 0xffffff33, 0xffccff33, 0xff99ff33, 0xff66ff33, 0xff33ff33, 0xff00ff33, 0xffffcc33,
	0xffcccc33, 0xff99cc33, 0xff66cc33, 0xff33cc33, 0xff00cc33, 0xffff9933, 0xffcc9933, 0xff999933,
	0xff669933, 0xff339933, 0xff009933, 0xffff6633, 0xffcc6633, 0xff996633, 0xff666633, 0xff336633,
	0xff006633, 0xffff3333, 0xffcc3333, 0xff993333, 0xff663333, 0xff333333, 0xff003333, 0xffff0033,
	0xffcc0033, 0xff990033, 0xff660033, 0xff330033, 0xff000033, 0xffffff00, 0xffccff00, 0xff99ff00,
	0xff66ff00, 0xff33ff00, 0xff00ff00, 0xffffcc00, 0xffcccc00, 0xff99cc00, 0xff66cc00, 0xff33cc00,
	0xff00cc00, 0xffff9900, 0xffcc9900, 0xff999900, 0xff669900, 0xff339900, 0xff009900, 0xffff6600,
	0xffcc6600, 0xff996600, 0xff666600, 0xff336600, 0xff006600, 0xffff3300, 0xffcc3300, 0xff993300,
	0xff663300, 0xff333300, 0xff003300, 0xffff0000, 0xffcc0000, 0xff990000, 0xff660000, 0xff330000,
	0xff000000, 0xffee0000, 0xffdd0000, 0xffbb0000, 0xffaa0000, 0xff880000, 0xff770000, 0xff550000,
	0xff440000, 0xff220000, 0xff110000, 0xff00ee00, 0xff00dd00, 0xff00bb00, 0xff00aa00, 0xff008800,
	0xff007700, 0xff005500, 0xff004400, 0xff002200, 0xff001100, 0xff0000ee, 0xff0000dd, 0xff0000bb,
	0xff0000aa, 0xff000088, 0xff000077, 0xff000055, 0xff000044, 0xff000022, 0xff000011, 0xffeeeeee,
	0xffdddddd, 0xffbbbbbb, 0xffaaaaaa, 0xff888888, 0xff777777, 0xff555555, 0xff444444, 0xff222222,
};

static void DefaultPaletteToRGBA(uint8_t* out) {
	// Index 0 is unused (empty voxel). Default palette entry 0 is also 0x00000000.
	// The .vox format maps RGBA[i] to color index i+1, but the default palette
	// is indexed directly by color index (0-255).
	for (int i = 0; i < 256; i++) {
		uint32_t c = kDefaultPaletteABGR[i];
		out[i * 4 + 0] = (c >> 0) & 0xFF;  // R
		out[i * 4 + 1] = (c >> 8) & 0xFF;  // G
		out[i * 4 + 2] = (c >> 16) & 0xFF; // B
		out[i * 4 + 3] = (c >> 24) & 0xFF; // A
	}
}

struct ChunkHeader {
	char id[4];
	uint32_t contentSize;
	uint32_t childrenSize;
};

static bool ReadChunkHeader(std::ifstream& f, ChunkHeader& h) {
	f.read(h.id, 4);
	f.read(reinterpret_cast<char*>(&h.contentSize), 4);
	f.read(reinterpret_cast<char*>(&h.childrenSize), 4);
	return f.good();
}

std::optional<VoxModel> LoadVoxFile(const std::string& path) {
	auto logger = spdlog::get("Render");

	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) {
		logger->error("VoxLoader: Cannot open file: {}", path);
		return std::nullopt;
	}

	// Read magic "VOX " and version
	char magic[4];
	uint32_t version;
	file.read(magic, 4);
	file.read(reinterpret_cast<char*>(&version), 4);

	if (std::memcmp(magic, "VOX ", 4) != 0) {
		logger->error("VoxLoader: Invalid magic number in {}", path);
		return std::nullopt;
	}

	logger->info("VoxLoader: Loading {} (version {})", path, version);

	// Read MAIN chunk header
	ChunkHeader mainHeader;
	if (!ReadChunkHeader(file, mainHeader) || std::memcmp(mainHeader.id, "MAIN", 4) != 0) {
		logger->error("VoxLoader: Missing MAIN chunk");
		return std::nullopt;
	}

	// Skip MAIN content (should be 0)
	if (mainHeader.contentSize > 0)
		file.seekg(mainHeader.contentSize, std::ios::cur);

	VoxModel model{};
	model.sizeX = model.sizeY = model.sizeZ = 0;
	model.volumeSize = 0;
	bool hasPalette = false;

	// Multi-model support: .vox files can contain multiple SIZE/XYZI pairs.
	// We accumulate all voxels, using the max bounding box for centering.
	struct SubModel {
		uint32_t sizeX, sizeY, sizeZ;
		struct Voxel { uint8_t x, y, z, colorIndex; };
		std::vector<Voxel> voxels;
	};
	std::vector<SubModel> subModels;
	SubModel currentSub{};

	// Parse child chunks
	auto childEnd = file.tellg() + static_cast<std::streamoff>(mainHeader.childrenSize);
	while (file.tellg() < childEnd && file.good()) {
		ChunkHeader chunk;
		if (!ReadChunkHeader(file, chunk)) break;

		auto chunkDataStart = file.tellg();

		if (std::memcmp(chunk.id, "SIZE", 4) == 0) {
			// Each SIZE starts a new sub-model
			currentSub = {};
			file.read(reinterpret_cast<char*>(&currentSub.sizeX), 4);
			file.read(reinterpret_cast<char*>(&currentSub.sizeY), 4);
			file.read(reinterpret_cast<char*>(&currentSub.sizeZ), 4);

		} else if (std::memcmp(chunk.id, "XYZI", 4) == 0) {
			uint32_t numVoxels;
			file.read(reinterpret_cast<char*>(&numVoxels), 4);
			currentSub.voxels.resize(numVoxels);
			file.read(reinterpret_cast<char*>(currentSub.voxels.data()), numVoxels * 4);

			if (numVoxels > 0) {
				subModels.push_back(std::move(currentSub));
			}
			currentSub = {};

		} else if (std::memcmp(chunk.id, "RGBA", 4) == 0) {
			// RGBA chunk: 256 entries, but palette[i] maps to color index i+1
			uint8_t rawPalette[256 * 4];
			file.read(reinterpret_cast<char*>(rawPalette), 256 * 4);

			// Index 0 is empty (transparent)
			model.palette[0] = 0;
			model.palette[1] = 0;
			model.palette[2] = 0;
			model.palette[3] = 0;

			// rawPalette[i] -> color index i+1
			for (int i = 0; i < 255; i++) {
				model.palette[(i + 1) * 4 + 0] = rawPalette[i * 4 + 0]; // R
				model.palette[(i + 1) * 4 + 1] = rawPalette[i * 4 + 1]; // G
				model.palette[(i + 1) * 4 + 2] = rawPalette[i * 4 + 2]; // B
				model.palette[(i + 1) * 4 + 3] = rawPalette[i * 4 + 3]; // A
			}
			hasPalette = true;
		}

		// Skip to end of chunk (content + children)
		file.seekg(chunkDataStart + static_cast<std::streamoff>(chunk.contentSize + chunk.childrenSize));
	}

	if (subModels.empty()) {
		logger->error("VoxLoader: No non-empty models found in {}", path);
		return std::nullopt;
	}

	if (!hasPalette) {
		logger->info("VoxLoader: No RGBA chunk, using default palette");
		DefaultPaletteToRGBA(model.palette.data());
	}

	// For multi-model files without scene graph, use the largest non-empty model.
	// (Scene graph transform parsing would be needed to correctly composite all models.)
	auto& best = *std::max_element(subModels.begin(), subModels.end(),
		[](const SubModel& a, const SubModel& b) { return a.voxels.size() < b.voxels.size(); });

	model.sizeX = best.sizeX;
	model.sizeY = best.sizeY;
	model.sizeZ = best.sizeZ;

	logger->info("VoxLoader: Using largest model ({}x{}x{}, {} voxels) from {} sub-models",
		model.sizeX, model.sizeY, model.sizeZ, best.voxels.size(), subModels.size());

	// Compute volume cube size: max dimension, rounded up to multiple of 8
	uint32_t maxDim = std::max({model.sizeX, model.sizeY, model.sizeZ});
	model.volumeSize = ((maxDim + 7) / 8) * 8;
	if (model.volumeSize < 8) model.volumeSize = 8;

	uint32_t vs = model.volumeSize;
	model.volume.resize(static_cast<size_t>(vs) * vs * vs, 0);

	// Center model in the volume
	int offsetX = (static_cast<int>(vs) - static_cast<int>(model.sizeX)) / 2;
	int offsetY = (static_cast<int>(vs) - static_cast<int>(model.sizeY)) / 2;
	int offsetZ = (static_cast<int>(vs) - static_cast<int>(model.sizeZ)) / 2;

	for (const auto& v : best.voxels) {
		int x = static_cast<int>(v.x) + offsetX;
		int y = static_cast<int>(v.y) + offsetY;
		int z = static_cast<int>(v.z) + offsetZ;

		// Volume layout: z * vs*vs + y * vs + x (matches imageStore(volume, ivec3(x,y,z), ...))
		model.volume[z * vs * vs + y * vs + x] = v.colorIndex;
	}

	logger->info("VoxLoader: Successfully loaded {}", path);
	return model;
}
