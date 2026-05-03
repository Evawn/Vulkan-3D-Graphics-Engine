#include "PaletteQuantizer.h"

#include <algorithm>

namespace voxel_bake {

PaletteQuantizer::PaletteQuantizer(const std::array<uint8_t, 256 * 4>& palette)
    : m_palette(palette)
{
    BuildLut();
}

void PaletteQuantizer::BuildLut() {
    // For each 4-bit-per-channel cell, find the nearest non-empty palette
    // entry by squared Euclidean distance in RGB. Cell-center color sampled
    // at (cellHi << 4) + 8 — the midpoint of the 16-step bucket — so the
    // lookup behaves the same regardless of which side of the bucket the
    // input falls on.
    for (uint32_t rh = 0; rh < 16; ++rh) {
        for (uint32_t gh = 0; gh < 16; ++gh) {
            for (uint32_t bh = 0; bh < 16; ++bh) {
                const int cr = static_cast<int>((rh << 4) | 0x08);
                const int cg = static_cast<int>((gh << 4) | 0x08);
                const int cb = static_cast<int>((bh << 4) | 0x08);

                int bestIdx = 1;
                int bestDist = 0x7FFFFFFF;
                // Skip index 0 — reserved as the empty-voxel sentinel.
                for (int i = 1; i < 256; ++i) {
                    const int pr = m_palette[i * 4 + 0];
                    const int pg = m_palette[i * 4 + 1];
                    const int pb = m_palette[i * 4 + 2];
                    const int dr = pr - cr;
                    const int dg = pg - cg;
                    const int db = pb - cb;
                    const int d  = dr * dr + dg * dg + db * db;
                    if (d < bestDist) {
                        bestDist = d;
                        bestIdx  = i;
                        if (d == 0) break;   // exact hit — short-circuit
                    }
                }

                const uint32_t key = (rh << 8) | (gh << 4) | bh;
                m_lut[key] = static_cast<uint8_t>(bestIdx);
            }
        }
    }
}

uint8_t PaletteQuantizer::QuantizeF(float r, float g, float b) const {
    auto toByte = [](float v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return Quantize(toByte(r), toByte(g), toByte(b));
}

} // namespace voxel_bake
