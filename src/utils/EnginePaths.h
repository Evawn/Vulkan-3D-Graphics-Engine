#pragma once

#include <filesystem>

// EnginePaths
//
// Centralizes well-known on-disk locations the engine reads/writes outside
// the user's chosen save paths. The cache directory is where the engine
// stages auto-managed artifacts that don't belong in resources/ (which is
// for shipped read-only assets). v1 anchors the cache to the project root;
// when we later switch to OS-conventional locations (e.g. ~/Library/Caches
// on macOS), all call sites stay unchanged because they go through these
// helpers.

namespace engine_paths {

// Returns the engine's cache directory. Creates it on demand — callers can
// freely write into the returned path without first checking existence.
std::filesystem::path GetEngineCacheDir();

// The convention path the Promote-to-scene workflow writes its baked
// animated voxel asset to, and the path CombinedRenderer's "Foliage VXA
// Path" parameter defaults to. This is the file-based contract that lets
// the producer (GltfImportTechnique) and consumer (CombinedRenderer)
// communicate without knowing about each other.
//
// Format: <cache>/promoted-foliage.vxa (with sibling _NNN.vox frames).
std::filesystem::path GetPromotedFoliagePath();

} // namespace engine_paths
