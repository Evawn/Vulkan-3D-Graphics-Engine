#include "VoxelColorSampler.h"

#include <algorithm>
#include <cmath>

namespace voxel_bake {

// ---- SampleBilinearRepeat ----
//
// glTF default sampler is REPEAT for both wrap_s and wrap_t. The half-texel
// offset (uv*size − 0.5) puts bilinear filter centers on texel centers —
// without it the result is biased by half a texel and edges of leaf alpha
// masks shift by ~one row/column.
//
// Wrapping uses a two-step modulo to handle negative `x % w`: in C++ the
// result keeps the sign of x, so we add w then take mod again. Most glTF UVs
// land in [0,1] so the slow path rarely fires, but tiled foliage atlases do
// step outside that range.
//
// ---- Alpha-weighted RGB filtering ----
//
// For RGB, this is *not* a plain bilinear blend. Foliage textures store the
// leaf as an RGB island with alpha=1, surrounded by a "gutter" with alpha=0
// and arbitrary (often near-black) RGB. A plain bilinear filter near the
// leaf edge mixes leaf RGB with gutter RGB — even when the alpha cutoff
// says "this voxel survives," the resulting RGB is darker than the leaf's
// actual color, producing the "ring of dark voxels around every leaf"
// artifact.
//
// The fix: weight each texel's RGB contribution by (bilinear_weight × alpha)
// and renormalize. Texels with alpha=0 contribute nothing to RGB regardless
// of where they fall in the filter footprint; only "valid" pixels feed into
// the result. This is equivalent to "premultiply RGB by alpha, bilinear
// filter, un-premultiply" — the standard trick used by alpha-aware mipmap
// downsamplers (NVTT, DirectXTex/TexConv).
//
// Alpha output stays a plain bilinear blend — the cutoff test wants to know
// "how much of the filter footprint is actually opaque," which is just the
// area-weighted alpha sum.
//
// Degenerate cases:
//   - All four texels have alpha=1 (interior of an opaque region): the
//     alpha weights cancel and the result equals plain bilinear.
//   - All four texels have alpha=0 (deep gutter): the renormalization
//     denominator is zero; we return RGB=0 and alpha=0 → caller's cutoff
//     check skips the voxel anyway.

glm::vec4 SampleBilinearRepeat(const gltf_import::Texture& tex, glm::vec2 uv) {
    if (tex.width == 0 || tex.height == 0 || tex.rgba8.empty()) {
        return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    auto wrap01 = [](float v) { return v - std::floor(v); };
    const float u = wrap01(uv.x) * static_cast<float>(tex.width);
    const float v = wrap01(uv.y) * static_cast<float>(tex.height);

    const float u0f = std::floor(u - 0.5f);
    const float v0f = std::floor(v - 0.5f);
    const int   x0 = static_cast<int>(u0f);
    const int   y0 = static_cast<int>(v0f);
    const float fx = (u - 0.5f) - u0f;
    const float fy = (v - 0.5f) - v0f;

    const int W = static_cast<int>(tex.width);
    const int H = static_cast<int>(tex.height);
    auto fetch = [&](int x, int y) -> glm::vec4 {
        x = ((x % W) + W) % W;
        y = ((y % H) + H) % H;
        const size_t i = (static_cast<size_t>(y) * tex.width + static_cast<size_t>(x)) * 4;
        return glm::vec4(
            tex.rgba8[i + 0], tex.rgba8[i + 1], tex.rgba8[i + 2], tex.rgba8[i + 3]
        ) * (1.0f / 255.0f);
    };
    const glm::vec4 c00 = fetch(x0,     y0);
    const glm::vec4 c10 = fetch(x0 + 1, y0);
    const glm::vec4 c01 = fetch(x0,     y0 + 1);
    const glm::vec4 c11 = fetch(x0 + 1, y0 + 1);

    // Bilinear corner weights.
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 =         fx  * (1.0f - fy);
    const float w01 = (1.0f - fx) *         fy;
    const float w11 =         fx  *         fy;

    // Alpha-weighted RGB. Each texel contributes (corner_weight × alpha) to
    // a numerator, normalized by the sum of (corner_weight × alpha).
    const float aw00 = w00 * c00.a;
    const float aw10 = w10 * c10.a;
    const float aw01 = w01 * c01.a;
    const float aw11 = w11 * c11.a;
    const float awSum = aw00 + aw10 + aw01 + aw11;

    glm::vec3 rgb(0.0f);
    if (awSum > 1e-6f) {
        rgb = (aw00 * glm::vec3(c00) +
               aw10 * glm::vec3(c10) +
               aw01 * glm::vec3(c01) +
               aw11 * glm::vec3(c11)) / awSum;
    }

    // Plain bilinear alpha — the cutoff test cares about coverage, not RGB
    // validity, so the standard area-weighted blend is the right thing.
    const float a = w00 * c00.a + w10 * c10.a + w01 * c01.a + w11 * c11.a;

    return glm::vec4(rgb, a);
}

namespace {

// ---- FlatColorSampler ----
//
// MaterialBaseColor: returns the material's RGB factor verbatim, plus its
// alpha. No texture, no quantization at the sampler level — the voxelizer
// quantizes the K-sample average for us.
//
// MayVaryAcrossTriangle() returns false → the voxelizer's K-sample loop
// short-circuits to K=1 for this sampler, so no work is wasted re-running
// the same lookup K times.

class FlatColorSampler final : public ColorSampler {
public:
    explicit FlatColorSampler(glm::vec4 factor)
        : m_rgb(glm::vec3(factor)), m_alpha(factor.a) {}
    bool MayVaryAcrossTriangle() const override { return false; }
    ColorSample Sample(const BaryHit&, uint32_t, uint32_t, uint32_t) const override {
        return { m_rgb, m_alpha };
    }
private:
    glm::vec3 m_rgb;
    float     m_alpha;
};

// ---- TextureColorSampler ----
//
// TextureSampled: bary-interpolate UV from the triangle's three vertex UVs,
// alpha-weighted bilinear texel fetch (REPEAT wrap), multiply by
// baseColorFactor, return un-quantized RGB + alpha.
//
// The alpha-cut decision lives in the voxelizer now — it has to operate on
// the K-sample mean alpha, which the sampler can't see. Same effective-
// cutoff rules as before (Mask/Blend respect material cutoff; Opaque uses
// 3/255 as a malformed-asset guard); the voxelizer reads alphaMode +
// alphaCutoff directly off the VoxelizePrimitive.

class TextureColorSampler final : public ColorSampler {
public:
    TextureColorSampler(const glm::vec2*             uvs,
                        const gltf_import::Texture*  tex,
                        glm::vec4                    factor)
        : m_uvs(uvs), m_tex(tex), m_factor(factor)
    {}

    bool MayVaryAcrossTriangle() const override { return true; }

    ColorSample Sample(const BaryHit& hit,
                       uint32_t i0, uint32_t i1, uint32_t i2) const override
    {
        const glm::vec2 uv = hit.u * m_uvs[i0]
                           + hit.v * m_uvs[i1]
                           + hit.w * m_uvs[i2];
        const glm::vec4 t = SampleBilinearRepeat(*m_tex, uv);
        const glm::vec4 c = t * m_factor;
        return { glm::vec3(c), c.a };
    }

private:
    const glm::vec2*            m_uvs    = nullptr;
    const gltf_import::Texture* m_tex    = nullptr;
    glm::vec4                   m_factor = glm::vec4(1.0f);
};

} // namespace

std::unique_ptr<ColorSampler> MakeSampler(const VoxelizePrimitive& prim,
                                          const VoxColorSource&    cs)
{
    // TextureSampled requested but missing requirements → quietly fall back to
    // flat. Keeps the voxelizer inner loop branch-free regardless of asset
    // quirks (e.g. one primitive in a multi-primitive GLB lacks UVs).
    const bool wantTexture = (cs.mode == VoxColorSource::Mode::TextureSampled);
    const bool canTexture  = (prim.uvs != nullptr) && (prim.baseColorTexture != nullptr);
    if (wantTexture && canTexture) {
        return std::make_unique<TextureColorSampler>(
            prim.uvs, prim.baseColorTexture, prim.baseColorFactor);
    }
    return std::make_unique<FlatColorSampler>(prim.baseColorFactor);
}

} // namespace voxel_bake
