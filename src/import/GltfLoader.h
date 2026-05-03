#pragma once

#include "MeshIR.h"

#include <optional>
#include <string>

// ---- GltfLoader ----
//
// Thin wrapper around cgltf that parses a .glb (or .gltf + bin) into MeshIR.
// All decoding work happens up front — by the time Load() returns, vertex /
// index / animation buffers are CPU-resident, embedded PNGs are decoded to
// RGBA8 via stb_image, and node hierarchy + skin joint indices are resolved
// to MeshIR-internal indices.
//
// v1 scope: skinned glTF 2.0 binary (.glb). Returns std::nullopt on parse
// failure; the panel surfaces this in the editor log. Non-skinned meshes are
// loaded fine — primitives without JOINTS_0 / WEIGHTS_0 zero-fill those slots
// so the same vertex layout applies uniformly.

namespace gltf_import {

struct LoadOptions {
    // If true, missing JOINTS_0 / WEIGHTS_0 attributes default to (joint=0,
    // weight=1) — i.e. the vertex follows the skin's first joint as if rigidly
    // parented. Lets non-skinned meshes flow through the skinned pipeline
    // without a separate code path. v1 always wants this on.
    bool fillMissingSkinningWithIdentity = true;
};

std::optional<MeshIR> LoadGlb(const std::string& path,
                              const LoadOptions& options = {});

} // namespace gltf_import
