# Multi-Sample Color Voxelization — Implementation Spec

Status: ready to implement
Owner: Evan
Last updated: 2026-05-03

## 1. Goal

Reduce per-voxel color noise in the texture-sampled bake (Milestone 5) by
supersampling: instead of one texture sample per voxel paint event, take K
samples distributed inside the voxel cube, average them in RGB space, and
quantize once at the end. K is exposed in the BakerPanel as a discrete
dropdown (1, 2, 4, 8, 16) with a default of 4.

This is a strict superset of current behavior — K=1 must produce a
byte-for-byte identical bake to today's master at the same parameters
(regression check).

## 2. Background — current state of the bake pipeline

A single voxel paint event today (in `Voxelize` in
[src/import/Voxelizer.cpp](../src/import/Voxelizer.cpp)) does:

1. Find a candidate cell that overlaps the triangle (SAT test).
2. Compute `BaryHit hit = ClosestPointOnTriangle(cellCenter, v0, v1, v2)`.
   This returns barycentric weights `(u, v, w)` summing to 1, plus the
   actual closest 3D point.
3. Compute `distSq = |hit.point - cellCenter|²` and gate on the
   nearest-triangle policy: `if (distSq >= bestDistSq[cellIndex]) continue`.
4. Call `sampler->Sample(hit, i0, i1, i2)`.
5. The sampler (today: `TextureColorSampler` in
   [src/import/VoxelColorSampler.cpp](../src/import/VoxelColorSampler.cpp))
   bary-interpolates UVs, bilinear-samples the texture, multiplies by
   `baseColorFactor`, runs an alpha-cut against `alphaMode`/`alphaCutoff`,
   and **returns a quantized palette index**.
6. Voxelizer writes the index into the cell.

That's a **point sample** of the texture per voxel. Adjacent voxels can pick
texels several pixels apart — fine if the texture is uniform, very noisy if
the texture has detail (leaf veins, bark cracks, leaf-color speckle).

## 3. Architectural change required

### 3.1 The sampler must return un-quantized RGB

The averaging step has to happen in RGB space, not palette-index space
(averaging palette indices is meaningless — they're categorical). So the
sampler interface changes to return continuous RGB; the voxelizer averages
K samples and quantizes once at the end.

The alpha-cut policy moves out of the sampler too — the voxelizer has to
apply it against the *mean* alpha across K samples, which the sampler can't
see.

### 3.2 The sampler exposes whether it varies across the triangle

The flat-color sampler returns the same RGB for any (u, v, w) — running it
K times is wasted work. A virtual hint method lets the voxelizer skip
the K>1 path for samplers whose output is constant.

## 4. File-by-file changes

### 4.1 [src/import/VoxelColorSampler.h](../src/import/VoxelColorSampler.h)

Replace the `ColorSample` struct and the `ColorSampler::Sample` signature:

```cpp
struct ColorSample {
    glm::vec3 rgb;     // un-premultiplied, [0,1]
    float     alpha;   // [0,1]
};

class ColorSampler {
public:
    virtual ~ColorSampler() = default;

    // Samplers whose output is constant for any (u,v,w) on the triangle
    // (e.g. FlatColorSampler) return false here so the voxelizer skips
    // the K-1 redundant per-cell samples — same RGB each time.
    virtual bool MayVaryAcrossTriangle() const { return false; }

    virtual ColorSample Sample(const BaryHit& hit,
                               uint32_t i0, uint32_t i1, uint32_t i2) const = 0;
};
```

`SampleBilinearRepeat` and `MakeSampler` declarations stay unchanged.

### 4.2 [src/import/VoxelColorSampler.cpp](../src/import/VoxelColorSampler.cpp)

**`FlatColorSampler`:**

```cpp
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
```

Note that `FlatColorSampler` no longer needs the `PaletteQuantizer` — it
just stores raw RGB. Update its constructor in `MakeSampler` accordingly
(drop the `q` argument).

**`TextureColorSampler::Sample`:** drop the alpha-cut and return raw RGB+α:

```cpp
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

bool MayVaryAcrossTriangle() const override { return true; }
```

The constructor can also drop the `q` argument and the `m_q` member —
quantization happens in the voxelizer now. Same for the `m_alphaMode` /
`m_alphaCutoff` members; those move to the voxelizer (it reads them off
`VoxelizePrimitive` directly).

**`MakeSampler`** loses its `q` parameter (callers update at §4.4):

```cpp
std::unique_ptr<ColorSampler> MakeSampler(const VoxelizePrimitive& prim,
                                          const VoxColorSource&    cs)
{
    const bool wantTexture = (cs.mode == VoxColorSource::Mode::TextureSampled);
    const bool canTexture  = (prim.uvs != nullptr) && (prim.baseColorTexture != nullptr);
    if (wantTexture && canTexture) {
        return std::make_unique<TextureColorSampler>(
            prim.uvs, prim.baseColorTexture, prim.baseColorFactor);
    }
    return std::make_unique<FlatColorSampler>(prim.baseColorFactor);
}
```

Update the corresponding declaration in `VoxelColorSampler.h`.

### 4.3 [src/import/Voxelizer.h](../src/import/Voxelizer.h)

Add the K knob:

```cpp
constexpr int kMaxSamplesPerVoxel = 16;

struct VoxelizeInput {
    // ... existing fields unchanged ...
    int samplesPerVoxel = 1;     // M5 supersampling. K=1 = current behavior.
};
```

### 4.4 [src/import/Voxelizer.cpp](../src/import/Voxelizer.cpp)

#### 4.4.1 Hardcoded jitter table

Add at file scope in the anonymous namespace, near `BaryHit`:

```cpp
// Halton(2, 3, 5) low-discrepancy sequence in [0,1]^3, shifted to [-0.5,
// 0.5]^3 (so multiplying by voxelSize gives an offset that stays inside
// the voxel cube around the cell center). 15 entries — enough for K up to
// kMaxSamplesPerVoxel=16, since sample 0 is always the closest-point
// (no jitter) and we use kJitterOffsets[0..K-2] for the remaining K-1.
//
// Halton is deterministic, so re-bakes are byte-identical given the same
// inputs. Generated offline; do not regenerate at runtime.
constexpr glm::vec3 kJitterOffsets[15] = {
    glm::vec3( 0.0000f, -0.1667f, -0.3000f),  // Halton(2,3,5) sample 1
    glm::vec3(-0.2500f,  0.1667f,  0.1000f),  // sample 2
    glm::vec3( 0.2500f, -0.3889f, -0.1000f),  // sample 3
    glm::vec3(-0.3750f, -0.0556f,  0.3000f),  // sample 4
    glm::vec3( 0.1250f,  0.2778f, -0.4600f),  // sample 5
    glm::vec3(-0.1250f, -0.2778f, -0.0600f),  // sample 6
    glm::vec3( 0.3750f,  0.0556f,  0.3400f),  // sample 7
    glm::vec3(-0.4375f,  0.3889f, -0.2200f),  // sample 8
    glm::vec3( 0.0625f, -0.4444f,  0.1800f),  // sample 9
    glm::vec3( 0.3125f,  0.2222f, -0.3800f),  // sample 10
    glm::vec3(-0.1875f, -0.1111f,  0.0200f),  // sample 11
    glm::vec3( 0.1875f,  0.4444f,  0.4200f),  // sample 12
    glm::vec3(-0.3125f, -0.3333f, -0.2600f),  // sample 13
    glm::vec3( 0.4375f,  0.1111f,  0.1400f),  // sample 14
    glm::vec3(-0.0625f, -0.4444f, -0.0200f),  // sample 15
};
```

These offsets are placeholders illustrating the *form* — the implementer
should generate a real Halton(2,3,5) sequence offline (any Python snippet:
`scipy.stats.qmc.Halton(d=3).random(15) - 0.5`) and replace the values
above. The exact numbers don't affect correctness, only sample distribution
quality. Any fixed deterministic 3D low-discrepancy set in [-0.5, 0.5]^3
works.

#### 4.4.2 Multi-sample inner loop

In `Voxelize`, replace the cell-paint section (currently the block ending
in `out.indices[li] = s.paletteIndex;`) with:

```cpp
const ColorSample s0 = sampler->Sample(hit, i0, i1, i2);

const int K = sampler->MayVaryAcrossTriangle()
            ? std::clamp(in.samplesPerVoxel, 1, kMaxSamplesPerVoxel)
            : 1;

glm::vec3 sumRgbA = s0.rgb * s0.alpha;
float     sumA    = s0.alpha;

for (int k = 1; k < K; ++k) {
    const glm::vec3 jPos = cellCenter + kJitterOffsets[k - 1] * voxelSize;
    const BaryHit   jh   = ClosestPointOnTriangle(jPos, v0, v1, v2);
    const ColorSample s  = sampler->Sample(jh, i0, i1, i2);
    sumRgbA += s.rgb * s.alpha;
    sumA    += s.alpha;
}

// Alpha-cut against the K-averaged alpha. Same effective-cutoff rules as
// the prior single-sample path: Mask/Blend respect the material's cutoff;
// Opaque uses a near-zero floor (3/255) as a malformed-asset guard.
const float avgAlpha = sumA / float(K);
const float effectiveCutoff =
    (prim.alphaMode == gltf_import::Material::AlphaMode::Opaque)
        ? (3.0f / 255.0f)
        : prim.alphaCutoff;
if (avgAlpha < effectiveCutoff) continue;     // skip — leave bestDistSq alone

// Alpha-weighted RGB average, then quantize once.
const glm::vec3 avgRgb = (sumA > 1e-6f)
    ? (sumRgbA / sumA)
    : glm::vec3(0.0f);
bestDistSq[li]  = distSq;
out.indices[li] = quantizer.QuantizeF(avgRgb.r, avgRgb.g, avgRgb.b);
```

Important detail: **don't** update `bestDistSq[li]` until after the alpha
cut passes. Skipping must leave the cell available for a farther opaque
triangle to claim — same nearest-tri/alpha-cut interaction we already have
for foliage cards in front of bark.

The `MakeSampler` call earlier in the loop also drops the `quantizer`
argument:

```cpp
std::unique_ptr<ColorSampler> sampler = MakeSampler(prim, in.colorSource);
```

### 4.5 [src/import/AnimationBaker.h](../src/import/AnimationBaker.h)

Add the K field to both job structs:

```cpp
struct PreviewBakeJob {
    // ... existing ...
    int samplesPerVoxel = 4;
};

struct FullBakeJob {
    // ... existing ...
    int samplesPerVoxel = 4;
};
```

### 4.6 [src/import/AnimationBaker.cpp](../src/import/AnimationBaker.cpp)

In both `RunPreview` and `RunFullBake`, before calling `Voxelize`, plumb
the K value into `VoxelizeInput`:

```cpp
in.samplesPerVoxel = job.samplesPerVoxel;
```

Place it next to the other `in.*` assignments (right above
`Voxelize(in, *m_quantizer, &cancel)`).

### 4.7 [src/rendering-techniques/gltf-import/GltfImportTechnique.h](../src/rendering-techniques/gltf-import/GltfImportTechnique.h)

Add to `GltfImportSession`:

```cpp
int samplesPerVoxel = 4;     // M5 supersampling default
```

Add to the public API on the technique class:

```cpp
void SetSamplesPerVoxel(int n);
int  GetSamplesPerVoxel() const { return m_session.samplesPerVoxel; }
```

### 4.8 [src/rendering-techniques/gltf-import/GltfImportTechnique.cpp](../src/rendering-techniques/gltf-import/GltfImportTechnique.cpp)

Definition for `SetSamplesPerVoxel` (place near `SetColorSource`):

```cpp
void GltfImportTechnique::SetSamplesPerVoxel(int n) {
    n = std::clamp(n, 1, voxel_bake::kMaxSamplesPerVoxel);
    if (n == m_session.samplesPerVoxel) return;
    m_session.samplesPerVoxel = n;
    // Reuse the voxel-size debounce path — same pattern as SetColorSource:
    // mark dirty + reset the timer so the next TickBakeState fires a re-
    // bake after the standard ~250ms debounce.
    m_voxelSizeDirty    = true;
    m_voxelSizeChangeAt = std::chrono::steady_clock::now();
}
```

In `SubmitPreviewBake` and `StartFullBake`, before submitting to the baker:

```cpp
job.samplesPerVoxel = m_session.samplesPerVoxel;
```

### 4.9 [src/editor/panels/BakerPanel.cpp](../src/editor/panels/BakerPanel.cpp)

In the Voxelization section, **immediately after** the Color source radio
block (just before the View mode toggle), add:

```cpp
// ---- Samples per voxel (M5 supersampling) ----
//
// Trades bake speed for noise reduction. K=1 is point-sampling (the
// behavior at the start of M5); K=4 is the recommended default; K=16 is
// overkill for most assets but useful when scrubbing high-frequency leaf
// textures. FlatColorSampler skips this path entirely (its output doesn't
// vary across the triangle) so Material mode is unaffected.
const int currentK = m_technique->GetSamplesPerVoxel();
const int kOptions[] = { 1, 2, 4, 8, 16 };
char kLabel[8];
std::snprintf(kLabel, sizeof(kLabel), "%d", currentK);
if (ImGui::BeginCombo("samples per voxel", kLabel)) {
    for (int k : kOptions) {
        char label[8];
        std::snprintf(label, sizeof(label), "%d", k);
        if (ImGui::Selectable(label, k == currentK)) {
            m_technique->SetSamplesPerVoxel(k);
        }
    }
    ImGui::EndCombo();
}
if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Texture samples averaged per voxel.\n"
                      "Higher = less noise, slower bake.\n"
                      "Default 4. Material mode ignores this.");
}
```

(`<cstdio>` is already included for `std::snprintf`.)

## 5. Defaults and rationale

| K  | Use case |
|----|----------|
| 1  | Regression / "show me the old behavior" |
| 2  | Marginal improvement, mostly skipped |
| 4  | **Default.** Standard MSAA-style ratio. |
| 8  | Visible improvement on dense leaf textures |
| 16 | Overkill, useful for fine bark detail |

K=4 is the standard "anti-aliasing default" — diminishing returns kick in
around K=8–16. Bake cost scales linearly with K (~100ms extra per K step
on the AnimatedOak preview).

## 6. Cost analysis

Per cell paint event at K=4: 3 extra `ClosestPointOnTriangle` calls (~30
dot products each) + 3 extra alpha-weighted bilinear samples. Roughly
~300 extra ops per cell.

AnimatedOak preview bake: ~500K cell paint events. K=4 adds ~50–100ms,
well within the 250ms debounce budget. K=16 quadruples that — 200–400ms
extra — which may exceed debounce on slower systems but stays usable for
full bakes.

Material mode is unaffected because `FlatColorSampler::MayVaryAcrossTriangle()`
returns false; the voxelizer short-circuits to K=1 internally.

## 7. Test plan

1. **Regression check:** bake the AnimatedOak at K=1 in Texture mode.
   Compare to `master` HEAD's bake (capture before the change). Must be
   byte-identical — the K=1 path should reproduce the old behavior exactly.
2. **Spatial noise:** bake at K=1, K=4, K=16 in Texture mode. Visually
   compare. K=4 should be visibly cleaner than K=1; K=16 noticeably cleaner
   still (with diminishing returns).
3. **Animation flicker:** run a full bake at K=4, play it back. Should be
   noticeably calmer than K=1 due to soft alpha-cut + sample averaging.
4. **Material mode:** toggle to Material, change K. Should have no visible
   effect on the bake (FlatColorSampler short-circuits).
5. **UI plumbing:** load a GLB, drag voxel size slider — bake should fire
   after debounce. Change K — bake should also fire after debounce. Both
   reuse the same dirty-flag path.
6. **Performance:** time K=4 vs K=1 preview on AnimatedOak. K=4 should fit
   comfortably under 250ms (the debounce window).

## 8. Gotchas

- **Don't update `bestDistSq[li]` before the alpha cut.** A K-sample mean
  alpha that fails the cutoff must leave the cell available for a farther
  opaque triangle — same invariant as the original single-sample alpha-cut
  policy. Updating `bestDistSq` early would punch holes through bark
  behind translucent leaves.
- **The first sample is "free."** The closest-point hit is already
  computed for the distance-gate test (steps 1–3 above happen *before*
  the new code). Reuse it as sample 0; iterate `kJitterOffsets[0..K-2]`
  for samples 1..K-1. The jitter table is intentionally 15 entries, not
  16, for this reason.
- **`MakeSampler` no longer takes the quantizer.** Quantization moved to
  the voxelizer. Update both the declaration and the call site.
- **`TextureColorSampler` constructor change.** Drops `q`, `alphaMode`,
  `alphaCutoff` arguments. Drop the corresponding members. Alpha mode
  and cutoff are read directly off `VoxelizePrimitive` in the voxelizer.
- **CMake `GLOB_RECURSE` re-glob:** no new files in this change, so a
  plain `cmake --build` from `build/` is sufficient. No reconfigure needed.

## 9. Out of scope (deliberately)

The following are real noise sources that **this change will not fully
address** — call them out in the commit message so future work has a clear
follow-up list:

- **Closest-triangle-flip** under animation (a voxel switching ownership
  between two adjacent leaf cards as the mesh deforms). Multi-sampling
  damps it slightly through the soft alpha cut, but doesn't eliminate it.
- **Palette quantization stair-stepping** (16M continuous RGB → 255
  discrete palette indices). K-averaging brings adjacent voxels' inputs
  closer together so they're more likely to land on the same palette
  entry, but the categorical-jump artifact remains.
- **Texture-content noise** (genuinely dark veins inside leaves). High K
  effectively low-passes the texture detail; that's a feature, not a bug,
  but if the user wants to preserve detail they'll set K=1 or K=2.
- **No temporal coupling between frames.** Each frame is voxelized
  independently; per-frame randomness in the closest-triangle decision
  isn't smoothed. A future "Approach E" (3-frame mode-filter post-pass)
  or "Approach F" (temporal coherence at sample time) would address this.
