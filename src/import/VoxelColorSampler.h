#pragma once

#include "Voxelizer.h"

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>

// ---- VoxelColorSampler ----
//
// Strategy object the voxelizer asks "what RGB does this triangle paint at
// this UV?" once per sample event. Hoisted out of the inner loop so the
// branch on VoxColorSource::Mode happens once per primitive — and so future
// modes (vertex color, dithering, OkLab) can drop in as new subclasses
// without touching the hot loop.
//
// Returns un-quantized RGB + alpha. The voxelizer averages K samples
// (alpha-weighted), applies the alpha-cut against the K-mean alpha, and
// quantizes once at the end. Quantization can't happen inside Sample()
// because averaging palette indices is meaningless — they're categorical,
// not continuous.

namespace voxel_bake {

class PaletteQuantizer;

// Output of one Sample() call: un-premultiplied RGB in [0,1] + alpha [0,1].
// The voxelizer uses alpha both to weight RGB contribution to the K-sample
// mean *and* as the value the cutoff test runs against.
struct ColorSample {
    glm::vec3 rgb   = glm::vec3(0.0f);
    float     alpha = 0.0f;
};

class ColorSampler {
public:
    virtual ~ColorSampler() = default;

    // Hint that lets the voxelizer skip the K>1 path for samplers whose
    // output is constant across the triangle (FlatColorSampler) — saves
    // K-1 redundant ClosestPointOnTriangle + texture fetches per cell.
    // Texture-driven samplers return true; flat colors return false.
    virtual bool MayVaryAcrossTriangle() const { return false; }

    // hit.u/v/w are barycentric weights of the closest point on the triangle
    // to the cell center (or to a jittered position inside the voxel cube
    // when supersampling); sum to 1. (i0, i1, i2) are the vertex indices of
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
                                          const VoxColorSource&    cs);

// Bilinear filter with REPEAT wrap (glTF default sampler) and alpha-weighted
// RGB blending — texels with α=0 contribute nothing to the RGB result, only
// to the bilinear alpha output. Exposed for tests and so future subclasses
// (OkLab/dithering) can reuse the texel fetch without reimplementing wrap
// math.
glm::vec4 SampleBilinearRepeat(const gltf_import::Texture& tex, glm::vec2 uv);

} // namespace voxel_bake
