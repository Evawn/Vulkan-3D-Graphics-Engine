#pragma once

#include <array>
#include <cstdint>

// The engine's canonical 256-entry RGBA8 palette. Used by every voxel
// technique that does material-index → color lookups (palette brickmap,
// instanced foliage, combined renderer). Lifted out of PaletteResource so
// callers without Vulkan deps (e.g. PrimitiveFactory) can build palettes
// that overlay onto this baseline rather than starting from zero — see
// docs/COMBINED-FOLIAGE-BLACK-BUG.md for the failure mode that motivated
// the split.
//
// Layout:
//   [0]       empty (alpha 0; never sampled — material 0 == "no voxel")
//   [1..9]    hardcoded shape colors (red/green/blue/yellow/orange/...).
//             Used by the procedural-shape brickmap technique.
//   [10..255] HSV rainbow sweep at fixed s=0.85, v=0.95. The foliage
//             technique deliberately picks indices 64..95 out of this band
//             (a green sweep) for its blade gradient.
//
// No Vulkan dependencies — pure host-side palette construction.
std::array<uint8_t, 256 * 4> BuildDefaultPalette();
