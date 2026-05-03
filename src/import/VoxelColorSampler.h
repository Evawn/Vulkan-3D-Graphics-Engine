#pragma once

#include "Voxelizer.h"

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>

// ---- VoxelColorSampler ----
//
// Strategy object the voxelizer asks for "what color (palette index) does this
// triangle paint here?" once per cell hit. Hoisted out of the inner loop so the
// branch on VoxColorSource::Mode happens once per primitive instead of once per
// voxel paint event — and so future modes (vertex color, dithering, OkLab) can
// drop in as new subclasses without touching the hot loop.
//
// The sampler is also where the alpha-cut policy lives. `ColorSample::skip` is
// the *correct* way to bail out a cell — distinct from "I painted index 0,"
// which would also be empty but would update bestDistSq[]. Skipping leaves
// bestDistSq[] unchanged so a slightly-farther opaque triangle behind the
// foliage card can still claim the cell on a later iteration. Otherwise alpha-
// cut leaves would punch unfillable holes through the bark.

namespace voxel_bake {

class PaletteQuantizer;

// Output of one Sample() call. `paletteIndex` is meaningful only when
// `skip == false`; on skip the voxelizer does nothing (no write, no distance
// update).
struct ColorSample {
    uint8_t paletteIndex = 0;
    bool    skip         = false;
};

class ColorSampler {
public:
    virtual ~ColorSampler() = default;
    // hit.u/v/w are barycentric weights of the closest point on the triangle
    // to the cell center; sum to 1. (i0, i1, i2) are the vertex indices of
    // the triangle the hit was computed against — the texture sampler uses
    // them to resolve per-vertex UVs from the primitive's UV array. The flat
    // sampler ignores them entirely.
    virtual ColorSample Sample(const BaryHit& hit,
                               uint32_t i0, uint32_t i1, uint32_t i2) const = 0;
};

// Build the sampler for one primitive. Falls back to FlatColorSampler when
// TextureSampled is requested but the primitive has no UVs or no texture —
// keeps the inner loop branch-free regardless of asset quirks.
std::unique_ptr<ColorSampler> MakeSampler(const VoxelizePrimitive& prim,
                                          const VoxColorSource&    cs,
                                          const PaletteQuantizer&  q);

// Bilinear filter with REPEAT wrap (glTF default sampler). Exposed for tests
// and so an OkLab/dithering subclass can reuse the texel fetch without
// reimplementing wrap math.
glm::vec4 SampleBilinearRepeat(const gltf_import::Texture& tex, glm::vec2 uv);

} // namespace voxel_bake
