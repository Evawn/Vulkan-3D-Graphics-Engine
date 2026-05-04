#pragma once

#include "MeshIR.h"

#include <atomic>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace voxel_bake {

class PaletteQuantizer;

// ---- BaryHit ----
//
// Barycentric coordinates + the closest point on the triangle to the query
// (cell center). The voxelizer computes this for every triangle-cell overlap
// using ClosestPointOnTriangle; the ColorSampler consumes (u, v, w) to
// interpolate per-vertex UVs at the closest point for texture sampling. The
// flat-color sampler ignores the contents — the hit is still computed because
// distance² (point − cellCenter) drives the nearest-triangle seam policy.

struct BaryHit { float u, v, w; glm::vec3 point; };

// ---- VoxColorSource ----
//
// Selects how a fragment of triangle painted into a voxel cell sources its
// RGB color. M3 ships MaterialBaseColor (flat material tint per primitive);
// M5 will add TextureSampled (barycentric-interpolated UV → texel fetch).
// The enum is wired through the Voxelizer's input today so M5 needs only an
// implementation switch — no API change.

struct VoxColorSource {
    enum class Mode : uint8_t {
        MaterialBaseColor,    // single color per primitive (M3)
        TextureSampled,       // bilinear texel fetch at the voxel's UV (M5)
    };
    Mode mode = Mode::MaterialBaseColor;
};

// ---- VoxFrame ----
//
// One frame's worth of voxel data — Z-slab packed (voxel (x,y,z) at byte
// `z * size.x * size.y + y * size.x + x`). M4 concatenates many of these into
// the procedural animated volume image. Index 0 means "empty"; 1..255 index
// into the palette the quantizer was built with.

struct VoxFrame {
    glm::uvec3            size = glm::uvec3(0);
    std::vector<uint8_t>  indices;        // size.x * y * z bytes
};

// ---- VoxelizePrimitive ----
//
// One drawable primitive's worth of geometry to feed the voxelizer. Vertices
// are expected to be already in world space — i.e. CPU-skinned. The caller
// (AnimationBaker) does the skinning step before calling Voxelize so the
// voxelizer itself stays geometry-only.
//
// `uvs` is optional today (MaterialBaseColor doesn't need it). Pass nullptr
// when the primitive has no UV stream; M5 falls back to MaterialBaseColor for
// such primitives even when texture sampling is requested.
//
// `baseColorTexture` is also optional — when set and colorSource.mode is
// TextureSampled, the M5 path samples it bilinearly using the barycentric-
// interpolated UV. The pointer is borrowed from a MeshIR held alive by the
// owning bake job (shared_ptr<const MeshIR>); the voxelizer never extends its
// lifetime, so callers must guarantee the IR outlives the Voxelize() call.
//
// `alphaMode` + `alphaCutoff` come straight from the source material. When
// alphaMode == Mask, the TextureColorSampler treats sampled-alpha < cutoff as
// "skip this voxel" — the cell is left for a farther opaque triangle to claim
// instead of being painted with a transparent texel and locking out bark
// behind a foliage card. Opaque/Blend modes paint regardless.

struct VoxelizePrimitive {
    const glm::vec3* positions     = nullptr;   // length = vertexCount
    const glm::vec2* uvs           = nullptr;   // length = vertexCount, may be null
    size_t           vertexCount   = 0;

    const uint32_t*  indices       = nullptr;   // length = indexCount
    size_t           indexCount    = 0;          // multiple of 3

    glm::vec4                    baseColorFactor   = glm::vec4(1.0f);
    const gltf_import::Texture*  baseColorTexture  = nullptr;
    gltf_import::Material::AlphaMode alphaMode    = gltf_import::Material::AlphaMode::Opaque;
    float                        alphaCutoff      = 0.5f;
};

// ---- VoxelizeInput ----
//
// The grid is sized as `ceil((max - min) / voxelSize)`. Caller is responsible
// for any safety margin (the bake pipeline adds a 1-voxel pad). Origin is
// inclusive on the min side, exclusive on the max side per the usual half-
// open AABB convention.

// Hard ceiling on supersampling, in case anyone's tempted to set it higher
// than the jitter table can serve. Bake cost scales linearly with K.
constexpr int kMaxSamplesPerVoxel = 16;

struct VoxelizeInput {
    const VoxelizePrimitive* primitives;
    size_t                   primitiveCount;

    glm::vec3       worldOriginMin = glm::vec3(0.0f);
    glm::vec3       worldOriginMax = glm::vec3(0.0f);
    float           voxelSizeWorld = 0.05f;
    VoxColorSource  colorSource{};

    // K-sample supersampling per voxel paint event. K=1 is point-sampling
    // (the pre-multisample behavior); higher K averages K closest-point /
    // bilinear samples taken at jittered positions inside the voxel cube,
    // which suppresses spatial noise from high-frequency texture detail.
    // Clamped to [1, kMaxSamplesPerVoxel] inside Voxelize().
    int             samplesPerVoxel = 1;
};

// ---- Voxelize ----
//
// Surface voxelization. For each triangle: compute its AABB in grid coords,
// iterate the candidate cells, run the Akenine-Möller separating-axis test
// to confirm intersection, then paint the cell with the triangle's color
// (palette-quantized).
//
// Seam policy: nearest-triangle. When two triangles paint the same cell, the
// one whose closest point on its surface lies closest to the cell center
// wins. A scratch float-per-cell distance buffer is allocated alongside the
// output indices and discarded on return — ~5% memory overhead during bake,
// dramatically nicer seams than first-write-wins.
//
// Cancellation: if `cancel` is non-null and becomes true, the function bails
// out between triangles. The returned VoxFrame is then a partial bake (still
// well-formed, just missing the un-processed triangles). Caller decides
// whether to discard or display the partial result.
//
// Threading: free of internal mutexes / globals. Multiple Voxelize calls can
// run concurrently on different threads given different output frames.

VoxFrame Voxelize(const VoxelizeInput&     in,
                  const PaletteQuantizer&  quantizer,
                  const std::atomic<bool>* cancel = nullptr);

// ---- Bake AABB sampling ----
//
// Compute the world-space AABB of the *posed* primitive set. M3 calls this
// once per preview bake (current pose only). M4 will call it K times across
// the clip duration and union the results to produce a clip-wide AABB so the
// per-frame Z-slabs can share a constant grid size.

struct AabbSample {
    glm::vec3 min = glm::vec3( std::numeric_limits<float>::max());
    glm::vec3 max = glm::vec3(-std::numeric_limits<float>::max());

    void Include(const glm::vec3& p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    bool valid() const { return min.x <= max.x; }
};

AabbSample ComputePosedAabb(const VoxelizePrimitive* prims, size_t primCount);

} // namespace voxel_bake
