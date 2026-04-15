#include "VoxLoader.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

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

// MagicaVoxel STRING = int32 length + bytes
static std::string ReadString(std::ifstream& f) {
	int32_t len = 0;
	f.read(reinterpret_cast<char*>(&len), 4);
	if (len <= 0) return {};
	std::string s(static_cast<size_t>(len), '\0');
	f.read(s.data(), len);
	return s;
}

// MagicaVoxel DICT = int32 num_pairs + [STRING key, STRING value] x N
static std::unordered_map<std::string, std::string> ReadDict(std::ifstream& f) {
	std::unordered_map<std::string, std::string> d;
	int32_t n = 0;
	f.read(reinterpret_cast<char*>(&n), 4);
	for (int32_t i = 0; i < n && f.good(); i++) {
		std::string k = ReadString(f);
		std::string v = ReadString(f);
		d.emplace(std::move(k), std::move(v));
	}
	return d;
}

// Parse "x y z" into ivec3. Returns (0,0,0) on failure.
static void ParseTranslation(const std::string& s, int32_t& x, int32_t& y, int32_t& z) {
	x = y = z = 0;
	std::istringstream iss(s);
	iss >> x >> y >> z;
}

// MagicaVoxel rotation byte encoding:
//   bits 0-1: column index of the non-zero entry in row 0   (0, 1, or 2)
//   bits 2-3: column index of the non-zero entry in row 1   (must differ from row 0)
//   bit    4: sign of row 0 (0 = +, 1 = -)
//   bit    5: sign of row 1
//   bit    6: sign of row 2
// Row 2's non-zero column is the remaining index: 3 - row0_col - row1_col.
// The identity rotation byte is 0b0000'0100 = 4 (row 0 at col 0, row 1 at col 1, ...).
struct Rot {
	int32_t col[3];   // non-zero column for each row
	int32_t sgn[3];   // ±1 per row
};
static Rot UnpackRot(uint8_t b) {
	Rot r{};
	r.col[0] = b & 3;
	r.col[1] = (b >> 2) & 3;
	r.col[2] = 3 - r.col[0] - r.col[1];
	r.sgn[0] = (b & 0x10) ? -1 : 1;
	r.sgn[1] = (b & 0x20) ? -1 : 1;
	r.sgn[2] = (b & 0x40) ? -1 : 1;
	return r;
}
// Apply rotation to an integer vector: out[i] = sgn[i] * v[col[i]].
static inline int32_t RotApplyAxis(const Rot& r, int32_t vx, int32_t vy, int32_t vz, int axis) {
	int32_t src = (r.col[axis] == 0) ? vx : (r.col[axis] == 1 ? vy : vz);
	return r.sgn[axis] * src;
}

// Scene graph node variants (we only retain what's needed for traversal)
struct SGTransform {
	int32_t child = -1;
	int32_t tx = 0, ty = 0, tz = 0;
	Rot rot{ {0,1,2}, {1,1,1} };   // default = identity
};
struct SGGroup     { std::vector<int32_t> children; };
struct SGShape     { std::vector<int32_t> model_ids; };
using SGNode = std::variant<SGTransform, SGGroup, SGShape>;

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
	model.volumeSize = glm::uvec3(0);
	bool hasPalette = false;

	// Multi-model support: .vox files can contain multiple SIZE/XYZI pairs,
	// positioned via a scene graph (nTRN/nGRP/nSHP). We parse the graph and
	// composite all sub-models into one volume.
	struct SubModel {
		uint32_t sizeX, sizeY, sizeZ;
		struct Voxel { uint8_t x, y, z, colorIndex; };
		std::vector<Voxel> voxels;
	};
	std::vector<SubModel> subModels;
	SubModel currentSub{};

	std::unordered_map<int32_t, SGNode> sceneNodes;

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

		} else if (std::memcmp(chunk.id, "nTRN", 4) == 0) {
			int32_t node_id = 0;
			file.read(reinterpret_cast<char*>(&node_id), 4);
			(void)ReadDict(file);                 // node attribs (unused)
			int32_t child_id = -1;
			file.read(reinterpret_cast<char*>(&child_id), 4);
			int32_t reserved = 0, layer_id = 0, num_frames = 0;
			file.read(reinterpret_cast<char*>(&reserved), 4);
			file.read(reinterpret_cast<char*>(&layer_id), 4);
			file.read(reinterpret_cast<char*>(&num_frames), 4);

			SGTransform t{};
			t.child = child_id;
			for (int32_t fi = 0; fi < num_frames && file.good(); fi++) {
				auto frame = ReadDict(file);
				if (fi == 0) {
					auto it = frame.find("_t");
					if (it != frame.end()) {
						ParseTranslation(it->second, t.tx, t.ty, t.tz);
					}
					auto itR = frame.find("_r");
					if (itR != frame.end()) {
						// "_r" is stored as a decimal string of the packed byte.
						int rv = 0;
						try { rv = std::stoi(itR->second); } catch (...) { rv = 4; }
						t.rot = UnpackRot(static_cast<uint8_t>(rv));
					}
				}
			}
			sceneNodes.emplace(node_id, SGNode{t});

		} else if (std::memcmp(chunk.id, "nGRP", 4) == 0) {
			int32_t node_id = 0;
			file.read(reinterpret_cast<char*>(&node_id), 4);
			(void)ReadDict(file);                 // node attribs
			int32_t num_children = 0;
			file.read(reinterpret_cast<char*>(&num_children), 4);
			SGGroup g{};
			g.children.resize(std::max(0, num_children));
			for (int32_t i = 0; i < num_children && file.good(); i++) {
				file.read(reinterpret_cast<char*>(&g.children[i]), 4);
			}
			sceneNodes.emplace(node_id, SGNode{std::move(g)});

		} else if (std::memcmp(chunk.id, "nSHP", 4) == 0) {
			int32_t node_id = 0;
			file.read(reinterpret_cast<char*>(&node_id), 4);
			(void)ReadDict(file);                 // node attribs
			int32_t num_models = 0;
			file.read(reinterpret_cast<char*>(&num_models), 4);
			SGShape s{};
			s.model_ids.reserve(std::max(0, num_models));
			for (int32_t i = 0; i < num_models && file.good(); i++) {
				int32_t model_id = 0;
				file.read(reinterpret_cast<char*>(&model_id), 4);
				(void)ReadDict(file);             // per-model attribs
				s.model_ids.push_back(model_id);
			}
			sceneNodes.emplace(node_id, SGNode{std::move(s)});

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

	// Walk the scene graph from root (node 0), accumulating transforms.
	// Composition of outer then inner: R_comp = R_outer ∘ R_inner,
	//                                  t_comp = t_outer + R_outer(t_inner).
	// Emit (model_id, t, R) for every nSHP reached.
	struct Placed { int32_t model_id; int32_t tx, ty, tz; Rot rot; };
	std::vector<Placed> placed;

	auto rotCompose = [](const Rot& outer, const Rot& inner) {
		Rot out{};
		for (int i = 0; i < 3; i++) {
			out.col[i] = inner.col[outer.col[i]];
			out.sgn[i] = outer.sgn[i] * inner.sgn[outer.col[i]];
		}
		return out;
	};
	auto rotApply = [](const Rot& r, int32_t vx, int32_t vy, int32_t vz, int32_t& ox, int32_t& oy, int32_t& oz) {
		ox = RotApplyAxis(r, vx, vy, vz, 0);
		oy = RotApplyAxis(r, vx, vy, vz, 1);
		oz = RotApplyAxis(r, vx, vy, vz, 2);
	};

	if (!sceneNodes.empty()) {
		struct Frame { int32_t node_id; int32_t tx, ty, tz; Rot rot; };
		std::vector<Frame> stack;
		stack.push_back({0, 0, 0, 0, Rot{{0,1,2},{1,1,1}}});
		while (!stack.empty()) {
			Frame fr = stack.back();
			stack.pop_back();
			auto it = sceneNodes.find(fr.node_id);
			if (it == sceneNodes.end()) continue;
			const SGNode& n = it->second;
			if (std::holds_alternative<SGTransform>(n)) {
				const auto& t = std::get<SGTransform>(n);
				// Child transform: t_new = fr.t + R_parent(t_child); R_new = R_parent ∘ R_child.
				int32_t rtx, rty, rtz;
				rotApply(fr.rot, t.tx, t.ty, t.tz, rtx, rty, rtz);
				Frame nf{t.child, fr.tx + rtx, fr.ty + rty, fr.tz + rtz, rotCompose(fr.rot, t.rot)};
				if (t.child >= 0) stack.push_back(nf);
			} else if (std::holds_alternative<SGGroup>(n)) {
				const auto& g = std::get<SGGroup>(n);
				for (int32_t c : g.children) stack.push_back({c, fr.tx, fr.ty, fr.tz, fr.rot});
			} else {
				const auto& s = std::get<SGShape>(n);
				for (int32_t mid : s.model_ids) placed.push_back({mid, fr.tx, fr.ty, fr.tz, fr.rot});
			}
		}
	}

	if (placed.empty()) {
		// No scene graph — place each sub-model at origin with identity rotation.
		Rot ident{{0,1,2},{1,1,1}};
		for (size_t i = 0; i < subModels.size(); i++) {
			placed.push_back({static_cast<int32_t>(i), 0, 0, 0, ident});
		}
	}

	// Compute world-space bbox over all placed sub-models.
	// For each: world(v) = t + R * (v - pivot), where pivot = size/2 integer.
	// A signed permutation R swaps/negates axes, so the rotated bbox is the
	// min/max over the 8 local corners transformed. Signed-permutation => min/max
	// per-axis occur at two opposite local corners, so just evaluating the
	// (0,0,0) and (sizeX, sizeY, sizeZ) corners and taking component-wise
	// min/max is sufficient.
	int32_t minX = INT32_MAX, minY = INT32_MAX, minZ = INT32_MAX;
	int32_t maxX = INT32_MIN, maxY = INT32_MIN, maxZ = INT32_MIN;
	for (const auto& p : placed) {
		if (p.model_id < 0 || static_cast<size_t>(p.model_id) >= subModels.size()) continue;
		const auto& sm = subModels[p.model_id];
		int32_t pivotX = static_cast<int32_t>(sm.sizeX) / 2;
		int32_t pivotY = static_cast<int32_t>(sm.sizeY) / 2;
		int32_t pivotZ = static_cast<int32_t>(sm.sizeZ) / 2;
		// Two opposite local corners relative to pivot.
		int32_t a[3] = { -pivotX,
		                 -pivotY,
		                 -pivotZ };
		int32_t b[3] = { static_cast<int32_t>(sm.sizeX) - pivotX,
		                 static_cast<int32_t>(sm.sizeY) - pivotY,
		                 static_cast<int32_t>(sm.sizeZ) - pivotZ };
		int32_t ra[3], rb[3];
		for (int i = 0; i < 3; i++) {
			ra[i] = RotApplyAxis(p.rot, a[0], a[1], a[2], i);
			rb[i] = RotApplyAxis(p.rot, b[0], b[1], b[2], i);
		}
		int32_t mnX = p.tx + std::min(ra[0], rb[0]);
		int32_t mnY = p.ty + std::min(ra[1], rb[1]);
		int32_t mnZ = p.tz + std::min(ra[2], rb[2]);
		int32_t mxX = p.tx + std::max(ra[0], rb[0]);
		int32_t mxY = p.ty + std::max(ra[1], rb[1]);
		int32_t mxZ = p.tz + std::max(ra[2], rb[2]);
		minX = std::min(minX, mnX); minY = std::min(minY, mnY); minZ = std::min(minZ, mnZ);
		maxX = std::max(maxX, mxX); maxY = std::max(maxY, mxY); maxZ = std::max(maxZ, mxZ);
	}

	model.sizeX = static_cast<uint32_t>(maxX - minX);
	model.sizeY = static_cast<uint32_t>(maxY - minY);
	model.sizeZ = static_cast<uint32_t>(maxZ - minZ);

	logger->info("VoxLoader: Composited {} placed shape(s) from {} sub-model(s), bbox = {}x{}x{}",
		placed.size(), subModels.size(), model.sizeX, model.sizeY, model.sizeZ);

	// Per-axis volume: pad each axis to a multiple of 8 (brick size).
	auto padTo8 = [](uint32_t n) { return std::max<uint32_t>(8u, (n + 7u) & ~7u); };
	model.volumeSize = glm::uvec3(
		padTo8(model.sizeX),
		padTo8(model.sizeY),
		padTo8(model.sizeZ)
	);
	uint32_t vsX = model.volumeSize.x;
	uint32_t vsY = model.volumeSize.y;
	uint32_t vsZ = model.volumeSize.z;
	model.volume.resize(static_cast<size_t>(vsX) * vsY * vsZ, 0);

	// Splat voxels: world(v) = t + R * (v - pivot), then offset by -worldMin
	// into the volume's local [0, vs) coord space.
	for (const auto& p : placed) {
		if (p.model_id < 0 || static_cast<size_t>(p.model_id) >= subModels.size()) continue;
		const auto& sm = subModels[p.model_id];
		int32_t pivotX = static_cast<int32_t>(sm.sizeX) / 2;
		int32_t pivotY = static_cast<int32_t>(sm.sizeY) / 2;
		int32_t pivotZ = static_cast<int32_t>(sm.sizeZ) / 2;
		for (const auto& v : sm.voxels) {
			int32_t lx = static_cast<int32_t>(v.x) - pivotX;
			int32_t ly = static_cast<int32_t>(v.y) - pivotY;
			int32_t lz = static_cast<int32_t>(v.z) - pivotZ;
			int32_t rx = RotApplyAxis(p.rot, lx, ly, lz, 0);
			int32_t ry = RotApplyAxis(p.rot, lx, ly, lz, 1);
			int32_t rz = RotApplyAxis(p.rot, lx, ly, lz, 2);
			int32_t x = p.tx + rx - minX;
			int32_t y = p.ty + ry - minY;
			int32_t z = p.tz + rz - minZ;
			if (x < 0 || y < 0 || z < 0) continue;
			if (x >= static_cast<int32_t>(vsX) || y >= static_cast<int32_t>(vsY) || z >= static_cast<int32_t>(vsZ)) continue;
			model.volume[static_cast<size_t>(z) * vsX * vsY + static_cast<size_t>(y) * vsX + static_cast<size_t>(x)] = v.colorIndex;
		}
	}

	logger->info("VoxLoader: Successfully loaded {}", path);
	return model;
}
