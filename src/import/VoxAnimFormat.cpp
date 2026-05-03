#include "VoxAnimFormat.h"

#include "Voxelizer.h"      // VoxFrame
#include "VoxLoader.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace voxel_bake {
namespace {

// =============================================================================
// .vox writer (minimal)
// =============================================================================
//
// MagicaVoxel format is well-documented and the subset we need for one-frame
// emission is just MAIN → SIZE + XYZI + RGBA. No scene graph (the file is
// always one model at the origin), no PACK chunk (single model), no extension
// chunks. ~80 lines.
//
// Coord convention: VoxLoader's read path puts voxel (x,y,z) at byte index
// `z*sizeX*sizeY + y*sizeX + x`, which is exactly how we hand the bake's
// VoxFrame.indices in. So we can write voxels directly without rotating.

struct VoxXYZIVoxel { uint8_t x, y, z, color; };

bool WriteChunkHeader(std::ofstream& f, const char id[4], uint32_t contentSize, uint32_t childrenSize) {
    f.write(id, 4);
    f.write(reinterpret_cast<const char*>(&contentSize), 4);
    f.write(reinterpret_cast<const char*>(&childrenSize), 4);
    return f.good();
}

// Pack one VoxFrame to disk. Skips empty voxels (color index 0). Palette is
// the writer's responsibility — every emitted .vox carries the bake's full
// 256-entry palette so the file is self-contained.

bool WriteVoxFile(const std::string& path,
                  const VoxFrame& frame,
                  const std::array<uint8_t, 256 * 4>& palette)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;

    // Magic + version.
    f.write("VOX ", 4);
    const uint32_t version = 150;
    f.write(reinterpret_cast<const char*>(&version), 4);

    // Build the XYZI list first so we know the chunk size.
    std::vector<VoxXYZIVoxel> filled;
    filled.reserve(frame.indices.size() / 8);     // sparse heuristic
    for (uint32_t z = 0; z < frame.size.z; ++z) {
        for (uint32_t y = 0; y < frame.size.y; ++y) {
            for (uint32_t x = 0; x < frame.size.x; ++x) {
                const size_t li = static_cast<size_t>(z) * frame.size.x * frame.size.y
                                + static_cast<size_t>(y) * frame.size.x
                                + static_cast<size_t>(x);
                const uint8_t c = frame.indices[li];
                if (c == 0) continue;
                filled.push_back({
                    static_cast<uint8_t>(x),
                    static_cast<uint8_t>(y),
                    static_cast<uint8_t>(z),
                    c
                });
            }
        }
    }

    // ---- MAIN ----
    // Content size = 0; children = SIZE + XYZI + RGBA chunk sizes (12 hdr +
    // contentSize each; XYZI carries a uint32 count + 4B per voxel; RGBA is
    // exactly 256*4 bytes of palette entries 1..255 + a trailing zero entry
    // per the MV spec — same layout the existing reader expects).
    const uint32_t sizeChunkBytes  = 12 + 12;
    const uint32_t xyziContent     = 4 + 4 * static_cast<uint32_t>(filled.size());
    const uint32_t xyziChunkBytes  = 12 + xyziContent;
    const uint32_t rgbaContent     = 256 * 4;
    const uint32_t rgbaChunkBytes  = 12 + rgbaContent;
    const uint32_t mainChildrenSz  = sizeChunkBytes + xyziChunkBytes + rgbaChunkBytes;

    if (!WriteChunkHeader(f, "MAIN", 0, mainChildrenSz)) return false;

    // ---- SIZE ----
    if (!WriteChunkHeader(f, "SIZE", 12, 0)) return false;
    f.write(reinterpret_cast<const char*>(&frame.size.x), 4);
    f.write(reinterpret_cast<const char*>(&frame.size.y), 4);
    f.write(reinterpret_cast<const char*>(&frame.size.z), 4);

    // ---- XYZI ----
    if (!WriteChunkHeader(f, "XYZI", xyziContent, 0)) return false;
    const uint32_t count = static_cast<uint32_t>(filled.size());
    f.write(reinterpret_cast<const char*>(&count), 4);
    if (count > 0) {
        f.write(reinterpret_cast<const char*>(filled.data()),
                static_cast<std::streamsize>(count * 4));
    }

    // ---- RGBA ----
    // VoxLoader treats palette[i] (1..255) as MV's rawPalette[i-1]. Mirror
    // that on write: emit raw[i-1] = palette[i*4..]. Slot 256 is unused per
    // spec, set to 0.
    if (!WriteChunkHeader(f, "RGBA", rgbaContent, 0)) return false;
    std::array<uint8_t, 256 * 4> raw{};
    for (int i = 0; i < 255; ++i) {
        raw[i * 4 + 0] = palette[(i + 1) * 4 + 0];
        raw[i * 4 + 1] = palette[(i + 1) * 4 + 1];
        raw[i * 4 + 2] = palette[(i + 1) * 4 + 2];
        raw[i * 4 + 3] = palette[(i + 1) * 4 + 3];
    }
    f.write(reinterpret_cast<const char*>(raw.data()), rgbaContent);

    return f.good();
}

// =============================================================================
// JSON manifest — hand-rolled writer + tolerant scanner-based reader.
// =============================================================================
//
// We don't pull in a JSON library because (a) the project has no JSON dep
// today, and (b) the .vxa schema is locked at 8 fields. The reader is
// forward-compatible: unknown fields are ignored, missing fields fall back to
// defaults. The writer always emits the same shape so a roundtrip is exact.
//
// If a richer manifest ever lands (per-frame metadata, multi-clip, etc.) it's
// the trigger to drop in nlohmann/json — at which point the public API of
// this module stays the same, only the internals change.

void WriteJsonString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << c;
                }
        }
    }
    os << '"';
}

bool WriteManifest(const std::string& path, const VxaManifest& m) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << "{\n";
    f << "  \"version\": "         << m.version       << ",\n";
    f << "  \"name\": ";          WriteJsonString(f, m.name);                    f << ",\n";
    f << "  \"frameCount\": "      << m.frameCount    << ",\n";
    f << "  \"fps\": "             << m.fps           << ",\n";
    f << "  \"voxelSizeWorld\": "  << m.voxelSizeWorld<< ",\n";
    f << "  \"size\": ["
        << m.size.x << ", " << m.size.y << ", " << m.size.z << "],\n";
    f << "  \"originWorldMin\": ["
        << m.originWorldMin.x << ", " << m.originWorldMin.y << ", " << m.originWorldMin.z << "],\n";
    f << "  \"originWorldMax\": ["
        << m.originWorldMax.x << ", " << m.originWorldMax.y << ", " << m.originWorldMax.z << "],\n";
    f << "  \"frames\": [\n";
    for (size_t i = 0; i < m.frames.size(); ++i) {
        f << "    ";
        WriteJsonString(f, m.frames[i]);
        f << (i + 1 == m.frames.size() ? "\n" : ",\n");
    }
    f << "  ]\n";
    f << "}\n";
    return f.good();
}

// ---- Manifest reader ----
//
// Tolerant single-pass scanner. Looks for `"key"` then reads the value:
// numbers (int / float), strings, and arrays of either. Whitespace and JSON
// commas/colons are skipped — comments are NOT supported (vanilla JSON).
// Unknown keys are silently consumed; missing keys keep manifest defaults.

struct Scanner {
    const std::string& src;
    size_t pos = 0;
    explicit Scanner(const std::string& s) : src(s) {}

    void skipWs() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                c == ',' || c == ':') { ++pos; continue; }
            break;
        }
    }

    bool eof() const { return pos >= src.size(); }

    char peek() { return pos < src.size() ? src[pos] : '\0'; }

    bool readString(std::string& out) {
        skipWs();
        if (peek() != '"') return false;
        ++pos;
        out.clear();
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                char n = src[pos + 1];
                switch (n) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    default:   out += n;    break;     // \uXXXX not needed for our shape
                }
                pos += 2;
            } else {
                out += src[pos++];
            }
        }
        if (pos < src.size()) ++pos;       // closing "
        return true;
    }

    bool readNumber(double& out) {
        skipWs();
        char* end = nullptr;
        const char* start = src.c_str() + pos;
        out = std::strtod(start, &end);
        if (end == start) return false;
        pos += static_cast<size_t>(end - start);
        return true;
    }

    bool readArrayOfNumbers(std::vector<double>& out) {
        skipWs();
        if (peek() != '[') return false;
        ++pos;
        out.clear();
        while (!eof()) {
            skipWs();
            if (peek() == ']') { ++pos; return true; }
            double v = 0.0;
            if (!readNumber(v)) return false;
            out.push_back(v);
        }
        return false;
    }

    bool readArrayOfStrings(std::vector<std::string>& out) {
        skipWs();
        if (peek() != '[') return false;
        ++pos;
        out.clear();
        while (!eof()) {
            skipWs();
            if (peek() == ']') { ++pos; return true; }
            std::string s;
            if (!readString(s)) return false;
            out.push_back(std::move(s));
        }
        return false;
    }

    // Skip a JSON value (object/array/scalar). Used to ignore unknown fields
    // without bailing on the whole parse.
    void skipValue() {
        skipWs();
        if (eof()) return;
        char c = peek();
        if (c == '"') { std::string s; readString(s); return; }
        if (c == '{' || c == '[') {
            const char open = c, close = (c == '{') ? '}' : ']';
            int depth = 0;
            while (!eof()) {
                char k = src[pos++];
                if (k == '"') {
                    // Skip embedded strings without counting their braces.
                    while (pos < src.size() && src[pos] != '"') {
                        if (src[pos] == '\\' && pos + 1 < src.size()) pos += 2;
                        else ++pos;
                    }
                    if (pos < src.size()) ++pos;
                } else if (k == open)  { ++depth; }
                else if (k == close) { --depth; if (depth == 0) return; }
            }
            return;
        }
        // Scalar: scan to next , } ] or end.
        while (!eof()) {
            char k = peek();
            if (k == ',' || k == '}' || k == ']') return;
            ++pos;
        }
    }
};

bool ReadManifest(const std::string& path, VxaManifest& outM) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return false;

    Scanner sc(text);
    sc.skipWs();
    if (sc.peek() != '{') return false;
    ++sc.pos;

    while (!sc.eof()) {
        sc.skipWs();
        if (sc.peek() == '}') { ++sc.pos; break; }

        std::string key;
        if (!sc.readString(key)) return false;
        sc.skipWs();
        if (sc.peek() == ':') ++sc.pos;
        sc.skipWs();

        if (key == "version") {
            double v; if (!sc.readNumber(v)) return false;
            outM.version = static_cast<int>(v);
        } else if (key == "name") {
            if (!sc.readString(outM.name)) return false;
        } else if (key == "frameCount") {
            double v; if (!sc.readNumber(v)) return false;
            outM.frameCount = static_cast<uint32_t>(v);
        } else if (key == "fps") {
            double v; if (!sc.readNumber(v)) return false;
            outM.fps = static_cast<float>(v);
        } else if (key == "voxelSizeWorld") {
            double v; if (!sc.readNumber(v)) return false;
            outM.voxelSizeWorld = static_cast<float>(v);
        } else if (key == "size") {
            std::vector<double> a; if (!sc.readArrayOfNumbers(a) || a.size() < 3) return false;
            outM.size = glm::uvec3(static_cast<uint32_t>(a[0]),
                                   static_cast<uint32_t>(a[1]),
                                   static_cast<uint32_t>(a[2]));
        } else if (key == "originWorldMin") {
            std::vector<double> a; if (!sc.readArrayOfNumbers(a) || a.size() < 3) return false;
            outM.originWorldMin = glm::vec3(static_cast<float>(a[0]),
                                            static_cast<float>(a[1]),
                                            static_cast<float>(a[2]));
        } else if (key == "originWorldMax") {
            std::vector<double> a; if (!sc.readArrayOfNumbers(a) || a.size() < 3) return false;
            outM.originWorldMax = glm::vec3(static_cast<float>(a[0]),
                                            static_cast<float>(a[1]),
                                            static_cast<float>(a[2]));
        } else if (key == "frames") {
            if (!sc.readArrayOfStrings(outM.frames)) return false;
        } else {
            // Unknown key — forward-compat path.
            sc.skipValue();
        }
    }
    return true;
}

} // namespace

// =============================================================================
// Public API
// =============================================================================

bool WriteVxa(const std::string&                     directory,
              const std::string&                     name,
              uint32_t                               frameCount,
              float                                  fps,
              float                                  voxelSizeWorld,
              const glm::vec3&                       worldOriginMin,
              const glm::vec3&                       worldOriginMax,
              const std::vector<VoxFrame>&           frames,
              const std::array<uint8_t, 256 * 4>&    palette)
{
    auto logger = spdlog::get("Render");

    if (frames.size() != frameCount) {
        if (logger) logger->error("WriteVxa: frames.size()={} != frameCount={}", frames.size(), frameCount);
        return false;
    }
    if (frameCount == 0) {
        if (logger) logger->error("WriteVxa: frameCount=0 — nothing to write");
        return false;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(directory, ec);   // best-effort; failure surfaces below on open()

    // Validate consistent frame size before writing anything — we'd rather
    // fail early than leave a half-written set on disk.
    const glm::uvec3 size = frames.front().size;
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].size != size) {
            if (logger) logger->error(
                "WriteVxa: frame {} size ({},{},{}) differs from frame 0 ({},{},{})",
                i, frames[i].size.x, frames[i].size.y, frames[i].size.z,
                size.x, size.y, size.z);
            return false;
        }
    }

    // Build manifest.
    VxaManifest m;
    m.version        = 1;
    m.name           = name;
    m.frameCount     = frameCount;
    m.fps            = fps;
    m.voxelSizeWorld = voxelSizeWorld;
    m.size           = size;
    m.originWorldMin = worldOriginMin;
    m.originWorldMax = worldOriginMax;
    m.frames.reserve(frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s_%03u.vox", name.c_str(), i);
        m.frames.emplace_back(buf);
    }

    // Per-frame .vox files first — if any fail, the manifest never lands and
    // the user can retry without a stale .vxa pointing at missing frames.
    for (uint32_t i = 0; i < frameCount; ++i) {
        const std::string voxPath = (fs::path(directory) / m.frames[i]).string();
        if (!WriteVoxFile(voxPath, frames[i], palette)) {
            if (logger) logger->error("WriteVxa: failed to write {}", voxPath);
            return false;
        }
    }

    const std::string manifestPath = (fs::path(directory) / (name + ".vxa")).string();
    if (!WriteManifest(manifestPath, m)) {
        if (logger) logger->error("WriteVxa: failed to write manifest {}", manifestPath);
        return false;
    }

    if (logger) logger->info("WriteVxa: wrote {} ({} frames @ {:.2f} fps, {}x{}x{})",
        manifestPath, frameCount, fps, size.x, size.y, size.z);
    return true;
}

std::optional<LoadedVxa> LoadVxa(const std::string& manifestPath) {
    auto logger = spdlog::get("Render");
    LoadedVxa out;

    if (!ReadManifest(manifestPath, out.manifest)) {
        if (logger) logger->error("LoadVxa: failed to parse manifest {}", manifestPath);
        return std::nullopt;
    }
    if (out.manifest.version != 1) {
        if (logger) logger->error("LoadVxa: unsupported version {} in {}", out.manifest.version, manifestPath);
        return std::nullopt;
    }
    if (out.manifest.frames.size() != out.manifest.frameCount) {
        if (logger) logger->error("LoadVxa: frames[] length {} != frameCount {} in {}",
            out.manifest.frames.size(), out.manifest.frameCount, manifestPath);
        return std::nullopt;
    }
    const glm::uvec3 size = out.manifest.size;
    if (size.x == 0 || size.y == 0 || size.z == 0) {
        if (logger) logger->error("LoadVxa: zero size in {}", manifestPath);
        return std::nullopt;
    }

    namespace fs = std::filesystem;
    const fs::path dir = fs::path(manifestPath).parent_path();

    // Concatenate every frame's bytes Z-sequentially. The image upload path
    // (AssetRegistry::UploadVolume) treats the buffer as one contiguous blob
    // of depth = size.z * frameCount, so frame i lives at byte offset
    // i * size.x * size.y * size.z.
    const size_t bytesPerFrame = static_cast<size_t>(size.x) * size.y * size.z;
    out.framesData.assign(bytesPerFrame * out.manifest.frameCount, 0);

    bool gotPalette = false;
    for (uint32_t i = 0; i < out.manifest.frameCount; ++i) {
        const std::string framePath = (dir / out.manifest.frames[i]).string();
        auto vox = LoadVoxFile(framePath);
        if (!vox) {
            if (logger) logger->error("LoadVxa: failed to load frame {} ({})", i, framePath);
            return std::nullopt;
        }
        // VoxLoader pads each axis to a multiple of 8 (brickmap convention).
        // For our bake-saved frames the sizes already match the manifest's,
        // because VoxLoader's pad-to-8 only inflates when the raw bbox isn't
        // already a multiple of 8 — but the manifest's `size` is the
        // authoritative grid we Z-pack into, so we read from VoxLoader's
        // padded volume into our manifest-sized blob using the manifest's
        // dims, ignoring any padding overshoot.
        if (vox->volumeSize.x < size.x || vox->volumeSize.y < size.y || vox->volumeSize.z < size.z) {
            if (logger) logger->error(
                "LoadVxa: frame {} dims ({},{},{}) smaller than manifest ({},{},{})",
                i, vox->volumeSize.x, vox->volumeSize.y, vox->volumeSize.z,
                size.x, size.y, size.z);
            return std::nullopt;
        }

        uint8_t* dst = out.framesData.data() + i * bytesPerFrame;
        for (uint32_t z = 0; z < size.z; ++z) {
            for (uint32_t y = 0; y < size.y; ++y) {
                const size_t srcRow = static_cast<size_t>(z) * vox->volumeSize.x * vox->volumeSize.y
                                    + static_cast<size_t>(y) * vox->volumeSize.x;
                const size_t dstRow = static_cast<size_t>(z) * size.x * size.y
                                    + static_cast<size_t>(y) * size.x;
                std::memcpy(dst + dstRow, vox->volume.data() + srcRow, size.x);
            }
        }

        if (!gotPalette) {
            out.palette = vox->palette;
            gotPalette = true;
        }
    }

    if (logger) logger->info("LoadVxa: loaded {} ({} frames @ {:.2f} fps, {}x{}x{})",
        manifestPath, out.manifest.frameCount, out.manifest.fps, size.x, size.y, size.z);
    return out;
}

} // namespace voxel_bake
