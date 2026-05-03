#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace voxel_bake {

struct VoxFrame;   // forward — defined in Voxelizer.h

// ---- VxaManifest ----
//
// In-memory mirror of a .vxa JSON manifest. The schema is intentionally tiny
// so v1 can hand-roll the JSON: 8 scalar/array fields plus a frames[] string
// list. Future schema changes should bump `version` and switch the reader on
// it — never silently reinterpret old files.
//
// `frames` holds basenames of per-frame .vox files relative to the manifest's
// directory, in playback order. The reader resolves them against the manifest
// path; the writer just emits "<name>_NNN.vox" entries.

struct VxaManifest {
    int            version           = 1;
    std::string    name;
    uint32_t       frameCount        = 0;
    float          fps               = 24.0f;
    float          voxelSizeWorld    = 0.0f;
    glm::uvec3     size              = glm::uvec3(0);
    glm::vec3      originWorldMin    = glm::vec3(0.0f);   // mesh-local AABB min
    glm::vec3      originWorldMax    = glm::vec3(0.0f);   // mesh-local AABB max
    std::vector<std::string> frames;                       // basenames, length = frameCount
};

// ---- WriteVxa ----
//
// Writes:
//   <directory>/<name>.vxa                   ← JSON manifest
//   <directory>/<name>_000.vox … _NNN.vox    ← per-frame voxel data
//
// Each .vox carries the same `palette` so the file is self-contained when
// loaded via the existing VoxLoader (which is also what tools like
// MagicaVoxel / Goxel can open for spot-checking individual frames).
//
// `frames.size()` MUST equal frameCount and every frame MUST share the same
// `size`. Returns false on the first I/O failure (manifest or any per-frame
// .vox); a partial write may be left on disk for inspection.

bool WriteVxa(const std::string&                     directory,
              const std::string&                     name,
              uint32_t                               frameCount,
              float                                  fps,
              float                                  voxelSizeWorld,
              const glm::vec3&                       worldOriginMin,
              const glm::vec3&                       worldOriginMax,
              const std::vector<VoxFrame>&           frames,
              const std::array<uint8_t, 256 * 4>&    palette);

// ---- LoadVxa ----
//
// Round-trip companion to WriteVxa. Parses the manifest, loads every per-
// frame .vox via the existing VoxLoader, validates dimensions match the
// manifest, and returns a single Z-slab-packed byte buffer + the metadata
// that callers feed into AssetRegistry::RegisterAnimatedVoxelAsset.
//
// The palette returned is the palette stored in the FIRST frame's .vox file
// — every frame is required to use the same one (the writer enforces this;
// the reader trusts it). If the first .vox lacks an RGBA chunk, the engine
// default palette is used (matching VoxLoader's existing fallback).

struct LoadedVxa {
    VxaManifest                  manifest;
    std::vector<uint8_t>         framesData;     // size.x * size.y * size.z * frameCount bytes
    std::array<uint8_t, 256 * 4> palette{};
};

std::optional<LoadedVxa> LoadVxa(const std::string& manifestPath);

} // namespace voxel_bake
