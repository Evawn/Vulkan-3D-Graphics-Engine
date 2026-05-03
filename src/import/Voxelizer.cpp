#include "Voxelizer.h"
#include "PaletteQuantizer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace voxel_bake {
namespace {

// Triangle-AABB overlap (Akenine-Möller, "Fast 3D Triangle-Box Overlap",
// JGT 2001). 13 separating axes:
//   - 3 box-aligned (X, Y, Z) — cheap rejection by triangle bbox vs box bbox
//   - 1 triangle plane test  — distance from box-center to plane vs projected radius
//   - 9 cross-products (e_i × box_axis_j) for i,j in {0,1,2}
// For each cross-product axis a = e × b_axis we compute:
//   p_i = a · v_i    (tri vertex projection)
//   r   = boxHalf · |a|
// Axis is separating iff min(p) > r OR max(p) < -r → no overlap.

bool TriangleBoxOverlap(const glm::vec3& boxCenter, const glm::vec3& boxHalf,
                        const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2)
{
    // Translate so box is at origin.
    const glm::vec3 u0 = v0 - boxCenter;
    const glm::vec3 u1 = v1 - boxCenter;
    const glm::vec3 u2 = v2 - boxCenter;

    // 1) Three box-aligned tests (fast bbox rejection).
    {
        float xmin = std::min({u0.x, u1.x, u2.x});
        float xmax = std::max({u0.x, u1.x, u2.x});
        if (xmin >  boxHalf.x || xmax < -boxHalf.x) return false;
        float ymin = std::min({u0.y, u1.y, u2.y});
        float ymax = std::max({u0.y, u1.y, u2.y});
        if (ymin >  boxHalf.y || ymax < -boxHalf.y) return false;
        float zmin = std::min({u0.z, u1.z, u2.z});
        float zmax = std::max({u0.z, u1.z, u2.z});
        if (zmin >  boxHalf.z || zmax < -boxHalf.z) return false;
    }

    // Triangle edges.
    const glm::vec3 e0 = u1 - u0;
    const glm::vec3 e1 = u2 - u1;
    const glm::vec3 e2 = u0 - u2;

    // 2) Triangle plane vs box.
    {
        const glm::vec3 n   = glm::cross(e0, e1);
        const float    d   = -glm::dot(n, u0);
        const float    rad = boxHalf.x * std::abs(n.x)
                            + boxHalf.y * std::abs(n.y)
                            + boxHalf.z * std::abs(n.z);
        if (std::abs(d) > rad) return false;
    }

    // 3) Nine cross-product axes. Each is a × box_axis where a is a triangle
    // edge. We expand the cross products since they're sparse — e.g.
    // edge × X-axis = (0, ez, -ey).
    auto separates = [](float p0, float p1, float p2, float r) -> bool {
        // True iff the projected triangle is entirely outside [-r, r].
        const float pmin = std::min({p0, p1, p2});
        const float pmax = std::max({p0, p1, p2});
        return pmin > r || pmax < -r;
    };

    // edge0 × X
    {
        float a =  e0.z, b = -e0.y;
        float p0 = a * u0.y + b * u0.z;
        float p1 = a * u1.y + b * u1.z;
        float p2 = a * u2.y + b * u2.z;
        float r  = boxHalf.y * std::abs(a) + boxHalf.z * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge0 × Y
    {
        float a = -e0.z, b =  e0.x;
        float p0 = a * u0.x + b * u0.z;
        float p1 = a * u1.x + b * u1.z;
        float p2 = a * u2.x + b * u2.z;
        float r  = boxHalf.x * std::abs(a) + boxHalf.z * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge0 × Z
    {
        float a =  e0.y, b = -e0.x;
        float p0 = a * u0.x + b * u0.y;
        float p1 = a * u1.x + b * u1.y;
        float p2 = a * u2.x + b * u2.y;
        float r  = boxHalf.x * std::abs(a) + boxHalf.y * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge1 × X
    {
        float a =  e1.z, b = -e1.y;
        float p0 = a * u0.y + b * u0.z;
        float p1 = a * u1.y + b * u1.z;
        float p2 = a * u2.y + b * u2.z;
        float r  = boxHalf.y * std::abs(a) + boxHalf.z * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge1 × Y
    {
        float a = -e1.z, b =  e1.x;
        float p0 = a * u0.x + b * u0.z;
        float p1 = a * u1.x + b * u1.z;
        float p2 = a * u2.x + b * u2.z;
        float r  = boxHalf.x * std::abs(a) + boxHalf.z * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge1 × Z
    {
        float a =  e1.y, b = -e1.x;
        float p0 = a * u0.x + b * u0.y;
        float p1 = a * u1.x + b * u1.y;
        float p2 = a * u2.x + b * u2.y;
        float r  = boxHalf.x * std::abs(a) + boxHalf.y * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge2 × X
    {
        float a =  e2.z, b = -e2.y;
        float p0 = a * u0.y + b * u0.z;
        float p1 = a * u1.y + b * u1.z;
        float p2 = a * u2.y + b * u2.z;
        float r  = boxHalf.y * std::abs(a) + boxHalf.z * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge2 × Y
    {
        float a = -e2.z, b =  e2.x;
        float p0 = a * u0.x + b * u0.z;
        float p1 = a * u1.x + b * u1.z;
        float p2 = a * u2.x + b * u2.z;
        float r  = boxHalf.x * std::abs(a) + boxHalf.z * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }
    // edge2 × Z
    {
        float a =  e2.y, b = -e2.x;
        float p0 = a * u0.x + b * u0.y;
        float p1 = a * u1.x + b * u1.y;
        float p2 = a * u2.x + b * u2.y;
        float r  = boxHalf.x * std::abs(a) + boxHalf.y * std::abs(b);
        if (separates(p0, p1, p2, r)) return false;
    }

    // No separating axis found — overlap.
    return true;
}

// ---- Closest point on triangle to a query point (Ericson, RTCD §5.1.5).
//
// Returns barycentric (u, v, w) with u + v + w = 1. The closest point is
// `u * a + v * b + w * c`. We use this to:
//   1. Get distance² from voxel center to triangle (nearest-triangle seam policy)
//   2. (M5) Interpolate per-vertex UVs at the closest point for texture sampling
//
// All Voronoi region cases handled explicitly — a touch verbose, but each
// branch is a couple of dot products and the function is hot.

struct BaryHit { float u, v, w; glm::vec3 point; };

BaryHit ClosestPointOnTriangle(const glm::vec3& p,
                               const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;

    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return { 1.0f, 0.0f, 0.0f, a };                 // vertex region A
    }

    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return { 0.0f, 1.0f, 0.0f, b };                 // vertex region B
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        const glm::vec3 pt = a + v * ab;
        return { 1.0f - v, v, 0.0f, pt };               // edge region AB
    }

    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return { 0.0f, 0.0f, 1.0f, c };                 // vertex region C
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        const glm::vec3 pt = a + w * ac;
        return { 1.0f - w, 0.0f, w, pt };               // edge region AC
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const glm::vec3 pt = b + w * (c - b);
        return { 0.0f, 1.0f - w, w, pt };               // edge region BC
    }

    // Inside-face region.
    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    const glm::vec3 pt = a + ab * v + ac * w;
    return { 1.0f - v - w, v, w, pt };
}

inline size_t LinearIndex(uint32_t x, uint32_t y, uint32_t z, const glm::uvec3& size) {
    return static_cast<size_t>(z) * size.x * size.y
         + static_cast<size_t>(y) * size.x
         + static_cast<size_t>(x);
}

} // namespace

AabbSample ComputePosedAabb(const VoxelizePrimitive* prims, size_t primCount) {
    AabbSample s;
    for (size_t pi = 0; pi < primCount; ++pi) {
        const auto& p = prims[pi];
        for (size_t i = 0; i < p.vertexCount; ++i) {
            s.Include(p.positions[i]);
        }
    }
    return s;
}

VoxFrame Voxelize(const VoxelizeInput& in,
                  const PaletteQuantizer& quantizer,
                  const std::atomic<bool>* cancel)
{
    VoxFrame out;
    if (in.voxelSizeWorld <= 0.0f || in.primitiveCount == 0) return out;

    const glm::vec3 extent = in.worldOriginMax - in.worldOriginMin;
    if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f) return out;

    // Grid sizing — ceil(extent / voxelSize), at least 1 in each axis.
    out.size = glm::uvec3(
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.x / in.voxelSizeWorld))),
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.y / in.voxelSizeWorld))),
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.z / in.voxelSizeWorld)))
    );

    const size_t totalCells = static_cast<size_t>(out.size.x) * out.size.y * out.size.z;
    out.indices.assign(totalCells, 0);

    // Nearest-triangle seam policy — keep the smallest distance² seen per
    // cell across all triangles that paint it. Discarded after the bake
    // returns.
    std::vector<float> bestDistSq(totalCells, std::numeric_limits<float>::infinity());

    const float voxelSize = in.voxelSizeWorld;
    const glm::vec3 boxHalf(voxelSize * 0.5f);
    const glm::vec3 origin = in.worldOriginMin;
    const glm::vec3 invVoxelSize(1.0f / voxelSize);

    for (size_t pi = 0; pi < in.primitiveCount; ++pi) {
        const auto& prim = in.primitives[pi];
        if (prim.indexCount < 3 || !prim.positions || !prim.indices) continue;

        // M3: flat material color quantized once per primitive.
        const glm::vec3 rgb = glm::vec3(prim.baseColorFactor);
        const uint8_t flatIdx = quantizer.QuantizeF(rgb.r, rgb.g, rgb.b);

        const size_t triCount = prim.indexCount / 3;
        for (size_t tri = 0; tri < triCount; ++tri) {
            // Cancellation check between triangles — fine-grained enough to
            // bail out within a few ms of a cancel signal at typical mesh
            // sizes (5K–50K triangles).
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return out;
            }

            const uint32_t i0 = prim.indices[tri * 3 + 0];
            const uint32_t i1 = prim.indices[tri * 3 + 1];
            const uint32_t i2 = prim.indices[tri * 3 + 2];
            if (i0 >= prim.vertexCount || i1 >= prim.vertexCount || i2 >= prim.vertexCount) continue;

            const glm::vec3 v0 = prim.positions[i0];
            const glm::vec3 v1 = prim.positions[i1];
            const glm::vec3 v2 = prim.positions[i2];

            // Triangle AABB in grid-cell coordinates.
            const glm::vec3 triMin = glm::min(glm::min(v0, v1), v2);
            const glm::vec3 triMax = glm::max(glm::max(v0, v1), v2);

            glm::ivec3 cellMin = glm::ivec3(glm::floor((triMin - origin) * invVoxelSize));
            glm::ivec3 cellMax = glm::ivec3(glm::floor((triMax - origin) * invVoxelSize));

            // Clamp to grid bounds. Triangles outside the grid (e.g. animation
            // overshoots the bake-wide AABB) are simply clipped.
            cellMin = glm::max(cellMin, glm::ivec3(0));
            cellMax = glm::min(cellMax, glm::ivec3(out.size) - 1);
            if (cellMax.x < cellMin.x || cellMax.y < cellMin.y || cellMax.z < cellMin.z) continue;

            for (int z = cellMin.z; z <= cellMax.z; ++z) {
                for (int y = cellMin.y; y <= cellMax.y; ++y) {
                    for (int x = cellMin.x; x <= cellMax.x; ++x) {
                        const glm::vec3 cellCenter = origin + (glm::vec3(x, y, z) + 0.5f) * voxelSize;
                        if (!TriangleBoxOverlap(cellCenter, boxHalf, v0, v1, v2)) continue;

                        const BaryHit hit = ClosestPointOnTriangle(cellCenter, v0, v1, v2);
                        const glm::vec3 d = hit.point - cellCenter;
                        const float distSq = glm::dot(d, d);

                        const size_t li = LinearIndex(x, y, z, out.size);
                        if (distSq >= bestDistSq[li]) continue;

                        bestDistSq[li]  = distSq;
                        // M3: flat material color. M5 will branch here on
                        // colorSource.mode and sample the texture using
                        // (hit.u, hit.v, hit.w) interpolated UVs.
                        out.indices[li] = flatIdx;
                    }
                }
            }
        }
    }

    return out;
}

} // namespace voxel_bake
