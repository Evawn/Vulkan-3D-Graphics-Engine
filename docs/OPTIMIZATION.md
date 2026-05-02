# InstancedVoxelTechnique — Performance Audit & Plan

A technical companion to [instanced.md](instanced.md) and
[VISION.md](VISION.md). Where instanced.md catalogues how the foliage
pillar will need to grow along *instance count* and *asset variety*, this
document is about the orthogonal axis: how fast each frame renders **at
the instance counts we already have**, and which optimizations move the
needle.

The headline finding is that the trace pass is **fragment-bound on inner-
DDA work, not vertex/draw bound on instance count**. That changes the
optimization priority away from VISION.md §2.2's scaling story (GPU
culling, draw-indirect, LOD) and toward per-fragment cost reductions
that don't require new infrastructure.

---

## 1. The measurement

At **16 384 grass instances** (`m_grid_dim = 128`,
[InstancedVoxelTechnique.cpp:454](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L454)):

| `m_max_iterations` | Trace-pass time |
| --- | --- |
| 128 | **~110 ms** |
| 10  | **~30 ms**  |

Same scene, same camera, only the inner-DDA iteration cap
(`uMaxIterations` in
[instanced_voxel_dda.glsl:79](../shaders/include/instanced_voxel_dda.glsl#L79))
varies between runs.

A 3.7× change in trace-pass time from a 12.8× change in iteration cap is
the entire diagnostic story. Everything in this document follows from
unpacking it.

---

## 2. What the measurement tells us

### 2.1 The DDA loop has correct natural termination

The inner DDA bails on AABB exit at
[instanced_voxel_dda.glsl:80](../shaders/include/instanced_voxel_dda.glsl#L80):

```glsl
if (any(lessThan(voxel, ivec3(0))) ||
    any(greaterThanEqual(voxel, meta.size))) break;
```

So `uMaxIterations` is a **safety cap**, not the natural terminator.
When it kicks in, it's because the ray hasn't yet exited the AABB and
hasn't hit a solid voxel — i.e., the cap throttles miss rays before they
finish their natural traversal.

### 2.2 Average miss-fragment was running ~37 iterations

If trace-pass time scales linearly with average inner-DDA iteration count
(plausible, since every other per-fragment cost is constant or
sub-dominant), then:

  - At cap=10:  total work ∝ 10 × N
  - At cap=128: total work ∝ avg_N × N
  - Ratio 110/30 ≈ 3.67 → avg_N ≈ 37

So under the prior 128-cap, the average miss fragment was traversing
about **37 voxels** of inner-DDA before the natural AABB exit terminated
it (or it hit a blade, but most fragments don't — see §2.3). That number
is consistent with rays entering a 16×32×16 AABB at typical screen-space
angles and walking until they exit.

### 2.3 Inner-DDA-on-misses is the dominant cost

The painted volume inside each instance's AABB is **~14% of the AABB
volume**: [instanced_voxel_generate.comp](../shaders/instanced_voxel_generate.comp)
distributes 7 blades around a ring of radius 2.2, with heights up to 14
(in a 16-tall asset). Painted bounds are roughly **10×10×15 voxels** out
of a **16×32×16** asset = 1500 / 8192 ≈ 18% (call it 14% under typical
sway).

That means **~86% of fragments inside any instance's rasterized AABB are
guaranteed misses** — they enter the AABB, traverse empty voxels, and
exit. Those misses pay the full average ~37-iteration DDA cost producing
nothing visible.

This is the single biggest fact about the current performance profile:
**most of the trace pass's GPU time is spent walking through empty space
inside cube AABBs that are 7× larger than the painted geometry they
contain**.

### 2.4 What this *doesn't* mean

The data above is about miss-fragment work. It does not directly measure:

  - **Shadow-trace cost.** The substrate's `traceShadowWorld`
    ([substrate.glsl:137](../shaders/include/substrate.glsl#L137)) runs
    only on hit fragments, with its own iteration cap of 512. At 14%
    hit-fragment density, shadow rays could still be a large absolute
    cost even though each one fires less frequently than the inner DDA.
  - **Overdraw.** The shader does `discard` *and* now writes
    `gl_FragDepth`, both of which disable early-Z, so overlapping AABBs
    each pay full DDA cost per pixel before any depth resolution. The
    cap-10 measurement reduces this proportionally but doesn't tease
    apart "many AABBs covering same pixel" from "one AABB rasterized to
    many pixels."
  - **Per-pass vertex/draw cost.** The pass dispatches one
    `vkCmdDraw(36, instanceCount, …)` per `RenderItem`
    ([InstancedVoxelTechnique.cpp:438-449](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L438-L449)),
    so 16K instances are one draw call from one cloud. CPU draw overhead
    is negligible at this scale.

The first two of those need separate measurements before we know their
weight relative to inner-DDA-on-misses. §6 lists what to instrument.

---

## 3. Tier-1 optimizations (cheap, high-impact, no architectural change)

These are changes that don't require new render-graph concepts, new data
structures, or rethinking the technique. Most are one-to-three-file
edits with clear expected impact.

### 3.1 Tighten the per-draw AABB to painted bounds

**Estimated impact: ~5–7× reduction in inner-DDA work.**

Currently the AABB push constant
([InstancedVoxelTechnique.cpp:440-441](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L440-L441))
is the asset's full volume bounds: `(0,0,0)..(16,32,16)`. The painted
geometry occupies roughly `(3,11,0)..(13,21,15)` in asset-local voxels.

Setting the AABB to the painted bounds gives:

  - **Fewer rasterized fragments**: AABB volume goes from 8192 to ~1500
    voxels — about **5.5× fewer covered pixels** at a typical view
    angle (fragment count scales roughly with AABB silhouette area,
    which scales with AABB diagonal).
  - **Shorter traversals on the misses that remain**: the average
    inner-DDA miss-traversal length drops from ~37 voxels to ~17
    voxels — about **2.2× fewer iterations per fragment**.

Combined: roughly **5× × 2× ≈ 10× less inner-DDA work**. At today's
30 ms (cap=10) baseline, that puts the inner-DDA cost in the single-
millisecond range and exposes whatever's underneath.

The painted bounds need to be propagated from the compute generator to
the host-side `RenderItem`. Two reasonable shapes:

  - **Constant per asset**: bake the painted bounds into the
    `AssetRegistry`'s asset descriptor at generation time. The asset's
    "footprint" is a known shape; precompute it.
  - **Computed per asset**: a one-time GPU reduction over the volume
    image after generation, written to a small SSBO the host reads on
    asset-load. More general (handles future, more dynamic assets) but
    overkill for v1.

V1 should be the constant-per-asset approach. The grass tuft's painted
bounds are a fixed function of the compute shader's parameters
(`kNumBlades`, ring radius, lean range, sway range, blade height range)
and don't change at runtime.

### 3.2 Lower `kShadowMaxDist` for foliage shadows

**Estimated impact: ~5–10× reduction in shadow-trace cost (size of cost
unknown until §3.4 measures it).**

Currently set to 64.0 world units in
[instanced_voxel.frag:194](../shaders/instanced_voxel.frag#L194):

```glsl
const float kShadowMaxDist = 64.0;   // world units
```

The substrate DDA caps at 512 voxel steps
([substrate.glsl:194](../shaders/include/substrate.glsl#L194)), and at
`kWorldVoxelSize = 0.0125`, 512 voxels = 6.4 m of ray. So
`kShadowMaxDist = 64.0` (= 5120 voxels of potential walk) doesn't
directly bound the iteration count — the 512-step cap does. But:

  - For shadow rays that escape into mostly-empty bricks, the cap is
    what terminates them, so they pay full 512 steps regardless of how
    "far" they're allowed to travel.
  - For shadow rays that find an occluder early, the distance cap is
    irrelevant.

The fix isn't a tweak to `kShadowMaxDist` — it's lowering **`kMaxSteps`
in substrate.glsl** from 512 to something like 64 or 128. At 64 steps,
shadows resolve correctly within ~0.8 m of the receiver (well past where
foliage shadows are perceptible) and walk **8× less** in the worst case.

This is a single-file change with the visible cost being shadows that
"escape into lit" past 0.8 m. For the foliage pillar specifically, that
is **fine** — distant grass shadows are not a feature. Terrain still
shadows itself via the brickmap pillar's own DDA.

### 3.3 Drop the inner-DDA cap default to ~24

**Estimated impact: small, but free.**

The user's empirical finding is that cap=10 is visually acceptable.
Cap=10 is aggressive — some far-into-AABB voxels are missed. A safer
default that still throttles outliers is **24** (covers most natural
traversals without throttling the long-axis-aligned ones).

This is just changing
[InstancedVoxelTechnique.h:116](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.h#L116)
default from 96 to 24. Already a slider, so artists can tune.

After §3.1 (AABB tightening), the natural traversal length drops to
~17 voxels and a cap of 24 is barely throttling at all — exactly what
you want a safety cap to do.

### 3.4 GPU timestamps per pass

**Estimated impact: zero on perf, infinite on diagnostic capability.**

Right now "the trace pass takes 110ms" is the only measurement we have.
We don't know:

  - How much is sky pre-pass vs. trace pass.
  - Within trace pass, how much is inner-DDA vs. shadow vs. overdraw.
  - Whether `m_compute_pass` (volume regenerate) is running every frame
    or only on parameter change.

A `vkCmdWriteTimestamp` pair around each pass + a small ImGui readout
would answer all three in one PR. This should land *first*, before any
of §3.1–§3.3, so the impact of each is observed cleanly.

---

## 4. Tier-2 optimizations (after Tier 1, if still over budget)

These are larger changes that should only be reached for if Tier 1
hasn't gotten us into budget. Each requires either new render-graph
machinery, new asset metadata, or non-trivial shader restructuring.

### 4.1 Depth pre-pass for early-Z recovery

**Estimated impact: ~2–3× on overdraw-dominated regions.**

Currently the trace pass writes color, depth, *and* runs the lighting
+ shadow path in one shader. With `discard` and `gl_FragDepth` both
disabling early-Z, every overlapping AABB pixel pays full DDA + shadow
cost before depth-test resolves the winner.

A two-pass split:

  1. **Depth pre-pass**: rasterize the cube AABBs, run inner DDA, on
     hit write `gl_FragDepth` and discard color; on miss `discard`. No
     lighting, no shadow trace.
  2. **Color pass**: same rasterization, depth-test `EQUAL`. Only the
     fragment that actually owns the pixel reaches the shader; the
     others early-Z out before any work.

Cost: ~one extra rasterization pass (cube rasterize + DDA, no shadow).
Benefit: shadow-trace and shading run **once per pixel**, not once per
overlapping AABB.

This becomes attractive only after AABB tightening (§3.1) — without
tightening, the overdraw is by overlapping cubes that mostly cover empty
space, and a pre-pass is expensive scaffolding. After tightening, every
covered cube pixel is more likely to actually be a hit, and the early-Z
gate is a clean win.

### 4.2 Coarse occupancy header in the asset volume

**Estimated impact: depends; bounded by §3.1 already eating the empty
fraction.**

The asset volume is sparse. A small per-asset 4³- or 8³-cell occupancy
header (one bit per cell of "any solid voxel?") would let the inner DDA
skip empty cells in big strides instead of one voxel at a time. The
brickmap pillar already does this at its own scale; the foliage pillar
could borrow the trick.

Worth doing only if §3.1 (AABB tightening) doesn't already collapse the
empty-fraction problem, which it largely does.

### 4.3 Per-instance LOD on shadow tracing

**Estimated impact: ~2× on a wide-shot frame; ~1× on a close-up.**

Skip the shadow trace for blades whose projected footprint is below a
threshold (so distant blades render as directly-lit only, no
self-shadow). The cost is a uniform "no shadow on far blades" look, but
foliage at silhouette range visually doesn't read its self-shadows
anyway.

The hook would be a per-instance distance check in
[instanced_voxel.frag](../shaders/instanced_voxel.frag) right before the
shadow trace, cheap to gate. Same idea as the larger LOD story in
[instanced.md §2.3](instanced.md) but scoped to shadow only.

---

## 5. Tier-3 (NOT first — what VISION.md mentions but doesn't help here)

VISION.md §2.2 lists three scaling-direction items for this pillar:

  1. GPU culling and draw-indirect.
  2. LOD via projected pixel size (impostor swap below threshold).
  3. Multi-species via descriptor indexing or per-species draws.

**None of these are the right next move under the current measurement.**
The reason is that all three address *vertex/draw-call* costs or
*per-instance bookkeeping* costs, and the current bottleneck is
*per-fragment* work.

  - **GPU culling** would help if vertex shader invocations were the
    bottleneck. They aren't — the technique already amortizes 16K
    instances into one `vkCmdDraw`, and 16K × 36 = 590K vertex shader
    invocations per frame is a tiny fraction of GPU capacity.
  - **Impostor LOD** is genuinely helpful for distant tufts but doesn't
    address the camera-close-to-tuft case where most pixels are
    rasterized at full resolution.
  - **Multi-species** is orthogonal to the perf problem — adds *more*
    fragment work per pixel, not less.

The right time to invest in those is *after* per-fragment work is in
budget, when scaling to 100K+ instances starts hitting vertex/draw
limits. The current measurement says we are several orders of magnitude
away from that regime.

---

## 6. Recommended sequence

In order, with each step gated on the previous one not already
having put us in budget:

  1. **§3.4 GPU timestamps** — instrument first, so every subsequent
     change is measured rather than guessed. (~0.5 day.)
  2. **§3.1 AABB tightening** — almost certainly the biggest single
     win. Bake painted bounds into the asset descriptor and propagate
     to the per-draw push constant. (~1 day, mostly plumbing.)
  3. **§3.2 Substrate `kMaxSteps` reduction** — one-line shader change
     once timestamps confirm shadow trace is meaningful weight in the
     post-3.1 profile. (~1 hour + visual QA.)
  4. **§3.3 Inner-DDA cap default lowered to 24** — cosmetic but
     correct after tightening. (~5 minutes.)
  5. **Stop and re-measure.** At this point we should be well under
     the 16ms budget at 16K instances. Likely no further work needed
     for v1 grass.
  6. If still over budget: **§4.1 depth pre-pass**. Significant
     restructuring; ~2–3 days.
  7. If still over budget: **§4.3 shadow-trace LOD by distance**. ~1
     day.

Sequence from 1–4 is roughly **2 working days** for what should be a
~10× improvement on the current trace-pass time.

---

## 7. Open questions

  - **What's the actual painted bounds of the asset?** §3.1 needs a
    concrete `(min, max)` per asset. Easy to derive from the compute
    generator's parameters; should be a constant baked into
    [InstancedVoxelTechnique.cpp](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp)
    next to where the asset is registered.
  - **Is the volume regenerate compute pass running every frame?**
    [InstancedVoxelTechnique.cpp:267](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L267)
    is the compute dispatch site; the comment chain says it's a v1
    simplification. Worth a timestamp before optimizing the trace
    pass — if compute is also expensive, gating it on
    parameter-change is a separate easy win.
  - **MSAA state.** `caps.msaaSamples` is wired through
    ([InstancedVoxelTechnique.cpp:111-115](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L111-L115)).
    With `discard` + `gl_FragDepth` and a thin-blade silhouette, sample-
    rate shading can multiply fragment cost by the sample count. Worth
    confirming whether it's 1× or 4×.
  - **What does a 60-fps budget even look like for this scene?** A 16ms
    frame budget shared across sky pre-pass, trace pass, brickmap pass,
    post-process chain, and editor UI suggests **~6–8ms for the trace
    pass at peak instance density**. Tier 1 should comfortably hit
    that; Tier 2 buys headroom for the next instance-count increment.

---

## 8. Reading order

  1. This document — diagnosis and plan.
  2. [VISION.md §2.2](VISION.md) — the foliage pillar's scaling story
     (separate from the perf work here).
  3. [instanced.md](instanced.md) — deep audit of the technique's
     current shape, including hooks the optimization work below
     interacts with.
  4. The code, starting with
     [instanced_voxel.frag](../shaders/instanced_voxel.frag) and
     [instanced_voxel_dda.glsl](../shaders/include/instanced_voxel_dda.glsl).
