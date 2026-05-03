#pragma once

#include <array>
#include <cstdint>

// ---- DefaultVoxPalette ----
//
// MagicaVoxel's default 256-entry palette, decoded into the renderer-friendly
// R,G,B,A byte layout. Index 0 is the "empty voxel" sentinel (RGBA = 0).
// Indices 1..255 follow the canonical MagicaVoxel layout (hue/saturation ramps
// in the bulk, greyscale ramp at the tail).
//
// One source of truth so every consumer agrees on what color index N means:
//   - VoxLoader  : fallback palette when a .vox file omits its RGBA chunk
//   - PaletteQuantizer : quantization target for the GLB→voxel bake pipeline
//   - Renderers  : palette resource bound to trace shaders
//
// Held as a constant std::array initialized at first use. Cheap to copy if a
// caller wants its own mutable copy (e.g. a future custom-palette workflow);
// most callers should reference it via const&.

namespace voxel {

extern const std::array<uint8_t, 256 * 4>& GetDefaultPalette();

} // namespace voxel
