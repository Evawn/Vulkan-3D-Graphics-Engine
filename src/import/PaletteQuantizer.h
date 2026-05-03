#pragma once

#include <array>
#include <cstdint>

// ---- PaletteQuantizer ----
//
// Maps an arbitrary 8-bit RGB color to its closest palette index in a 256-entry
// RGBA palette (typical: the MagicaVoxel default — see DefaultVoxPalette.h).
// Built once at construction; `Quantize(r,g,b)` is a couple of shifts and a
// lookup after that.
//
// Strategy: precompute a 16×16×16 = 4096-entry LUT keyed by the high 4 bits of
// each channel. For each LUT cell, the centroid color (cell-center RGB) is
// nearest-matched against every non-empty palette entry; the winning index is
// stored in the LUT. At quantize time we just shift the input down to 4 bits
// per channel, combine into a 12-bit key, and read the LUT.
//
// Why a 4096-cell LUT (vs 256 or 65k):
//   - 256 cells (3-bit-per-channel) loses a lot of color resolution — adjacent
//     palette entries collapse into the same key.
//   - 65k cells (5-bit-per-channel) hits cache-line pressure during bakes
//     (200KB+) and the resolution gain over 4096 is barely visible after the
//     palette quantization anyway.
//   - 4096 fits comfortably in L1 (~4KB), gives every palette entry a distinct
//     LUT region, and the build cost (~1M comparisons) is sub-millisecond.
//
// Empty sentinel: palette index 0 is reserved for "empty voxel" and is NOT a
// quantization target — Quantize always returns an index in [1, 255]. Callers
// that want to mark a cell empty write 0 directly (e.g. on the per-frame
// volume clear), independent of any quantize call.
//
// Distance metric: squared Euclidean in RGB (cheap, decent for the v1 default
// palette which is mostly hue/sat ramps + greyscale). Future improvements
// (perceptual distance via OkLab, dithering across voxels) replace this
// internal without touching callers.

namespace voxel_bake {

class PaletteQuantizer {
public:
    // Build the LUT against the given palette (R,G,B,A bytes — alpha unused).
    // Index 0 is treated as the empty sentinel and excluded from the search.
    explicit PaletteQuantizer(const std::array<uint8_t, 256 * 4>& palette);

    // Map an 8-bit RGB triple to a palette index in [1, 255]. Branch-free
    // after the LUT is built — three bit-shifts, an OR, and a memory read.
    uint8_t Quantize(uint8_t r, uint8_t g, uint8_t b) const {
        const uint32_t key = ((uint32_t)(r >> 4) << 8)
                           | ((uint32_t)(g >> 4) << 4)
                           |  (uint32_t)(b >> 4);
        return m_lut[key];
    }

    // Convenience — float in [0,1] domain. Clamps before truncating to bytes.
    uint8_t QuantizeF(float r, float g, float b) const;

    // Direct palette read — useful for debug/inspector tooling that wants to
    // see "what color did index N quantize to" without round-tripping through
    // the LUT.
    void PaletteColor(uint8_t index, uint8_t& r, uint8_t& g, uint8_t& b) const {
        r = m_palette[index * 4 + 0];
        g = m_palette[index * 4 + 1];
        b = m_palette[index * 4 + 2];
    }

private:
    std::array<uint8_t, 4096>      m_lut{};
    std::array<uint8_t, 256 * 4>   m_palette{};

    void BuildLut();
};

} // namespace voxel_bake
