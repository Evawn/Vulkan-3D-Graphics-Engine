# CombinedRenderer — Performance Audit & Short-Term Plan

A technical companion to the older [OPTIMIZATION.md](OPTIMIZATION.md) and
[instanced.md](instanced.md). Those documents diagnosed the
`InstancedVoxelTechnique` (foliage-only) at the time when foliage shadows
were a per-cloud shadow map and there was no shared substrate. Most of
their headline architectural recommendations have since landed —
specifically the world-grid occupancy substrate from
[VISION.md §3](VISION.md), realized as the
[ShadowBrickmap](../src/rendering/voxel/ShadowBrickmap.h) — so the bottlenecks
they audited are not the bottlenecks this technique faces today.

This document audits the *current* `CombinedRenderer` shape and
re-prioritizes which advice from the older docs still moves the needle on
short-term performance, along with new findings specific to the
combined technique.

The scope is **short-term** — the next ~1–2 weeks of work to push the
technique from "useable" to "acceptable" at the current scene
(~4 K foliage instances, 1024×1024 island terrain). It is **not** about
scaling to millions of instances; that's a separate axis with separate
work (and is largely covered by [instanced.md §2.2–2.3](instanced.md)).

---

## 1. Current architecture

The `CombinedRenderer` is a single technique
([CombinedRenderer.h:46](../src/rendering-techniques/combined/CombinedRenderer.h#L46))
that owns both pillars from [VISION.md §2](VISION.md) — CPU-baked island
terrain *and* a procedurally animated foliage cloud — and renders them
through a shared world-grid occupancy substrate. Both pillars compute
their sun-shadow ray with the **same** `traceShadowWorld`
([shadow_trace.glsl:39](../shaders/include/shadow_trace.glsl#L39)) over
the same shadow brickmap, so terrain shadows fall on grass and grass
shadows fall on terrain — voxel-precise and symmetric.

### 1.1 Coordinate convention

Load-bearing across the technique:

  - One world voxel pitch:
    `kWorldVoxelSize = 0.0254` (= 1 inch /
    [CombinedRenderer.cpp:33](../src/rendering-techniques/combined/CombinedRenderer.cpp#L33)).
  - Terrain centered at world origin: `terrain.originVoxel = -volumeSize/2`
    ([CombinedRenderer.cpp:729](../src/rendering-techniques/combined/CombinedRenderer.cpp#L729)).
  - Foliage cloud anchored at world origin: `cloudWorld = scale(I, kWorldVoxelSize)`
    ([CombinedRenderer.cpp:605](../src/rendering-techniques/combined/CombinedRenderer.cpp#L605)).

The cloud-local-equals-world-voxel identity is what lets the unified
shadow query test terrain bricks via the same `voxel` variable it walks
the substrate with. Any change to that anchoring breaks shadow correctness
([combined_foliage_trace.frag:156-158](../shaders/combined_foliage_trace.frag#L156-L158)).

### 1.2 Pass list

In execution order:

| # | Type      | Pass                               | What it does |
|---|-----------|------------------------------------|--------------|
| 1 | Compute   | **Foliage Generate**               | Re-runs `instanced_voxel_generate.comp` every frame: writes the (16×32×16)×8-frame `R8_UINT` voxel image *and* a per-frame asset bitmask SSBO consumed by pass 2. |
| 2 | Compute   | **Shadow Foliage Write**           | One workgroup per (instance, world-brick) pair. 8×8×8 = 512 threads/group atomic-OR the dynamic-pool bits of the shadow brickmap from the asset bitmask at this instance's current animation phase. Preceded by a `vkCmdFillBuffer` clear of the dynamic pool. |
| 3 | Graphics  | **Sky pre-pass**                   | Fullscreen quad that clears color to a sky gradient + sun disk. `LoadOp::Clear` color, no depth. |
| 4 | Graphics  | **Terrain Trace**                  | Fullscreen quad. Two-level DDA over the terrain palette brickmap. On hit: shade with `traceShadowWorld` for sun-shadow. Writes `gl_FragDepth`; `discard` on miss so sky shows through. |
| 5 | Graphics  | **Foliage Trace**                  | Instanced 36-vert cubes (one cube per foliage instance, AABB rasterization). Per-fragment inner-DDA against the per-instance volume image; on hit shade with `traceShadowWorld`. Writes `gl_FragDepth`, depth-tested against terrain. |

### 1.3 The shadow brickmap

This is the most consequential change since OPTIMIZATION.md was written
and the part of the architecture that retired the largest old bottleneck.

[ShadowBrickmap.h](../src/rendering/voxel/ShadowBrickmap.h) is a
dedicated **occupancy-only** acceleration structure, separate from the
terrain palette brickmap. It packs three layers in one buffer:

```
[0..7]                             header (gridDim, brickSize, brickCount, originVoxel)
[topGridBase..]                    top-level grid: brick id or kEmptyBrick
[staticPoolBase..]                 brickCount × 16 words: terrain bits  (bake once)
[dynamicPoolBase..]                brickCount × 16 words: foliage bits  (rewrite/frame)
```

The shadow query reads `static_bit | dynamic_bit` per voxel
([shadow_brickmap.glsl:80-82](../shaders/include/shadow_brickmap.glsl#L80-L82)),
which means:

  - **Per-frame work touches only the dynamic pool.** No need to "preserve"
    static across the rewrite — they're at disjoint word offsets.
  - **Empty-brick skipping is real.** One outer step in the shadow trace
    advances 8 voxels of distance for the cost of one SSBO read. Dense
    brickmaps (few empty bricks) lose this win; for our open-air scene,
    the vast majority of shadow rays hit empty bricks.
  - **Inner cap is geometric**, not heuristic: 24 voxel steps = 3×brick
    size = the worst-case corner-to-corner traversal of an 8³ brick
    ([shadow_trace.glsl:124](../shaders/include/shadow_trace.glsl#L124)).
  - **Outer cap is a tunable** (`maxShadowBrickSteps`, default 256;
    [CombinedRenderer.cpp:143](../src/rendering-techniques/combined/CombinedRenderer.cpp#L143)).

This is the realization of the "world-grid occupancy substrate" promised
by VISION.md §3, and it makes the OPTIMIZATION.md §3.2 advice
("lower `kMaxSteps` from 512 to 64–128") both obsolete in mechanism
and **still relevant in spirit** — see §4.2 below.

### 1.4 Per-frame data flow

```
              ┌──────────────────────────┐
              │ Foliage Generate (CS)    │  writes  volume image + asset bitmask
              └────────────┬─────────────┘
                           │
              ┌────────────▼─────────────┐
              │ Shadow Foliage Write (CS)│  fills    shadow dynamic pool
              └────────────┬─────────────┘     (atomicOr from instance × asset bitmask)
                           │
                           │   ── instance buffer (CPU-staged, persistent) ──┐
                           │                                                  │
              ┌────────────▼─────────────┐                                    │
              │ Sky pre-pass (FS)        │  clears   color                    │
              └────────────┬─────────────┘                                    │
                           │                                                  │
              ┌────────────▼─────────────┐  reads   terrain brickmap          │
              │ Terrain Trace (FS)       │  reads   shadow brickmap           │
              │                          │  writes  color, gl_FragDepth       │
              └────────────┬─────────────┘                                    │
                           │                                                  │
              ┌────────────▼─────────────┐  reads   volume image, instance ◄──┘
              │ Foliage Trace (Inst.)    │  reads   shadow brickmap
              │                          │  writes  color, gl_FragDepth
              └──────────────────────────┘
```

### 1.5 What's CPU vs GPU

  - **Terrain bake** — CPU, on-demand
    ([CombinedRenderer.cpp:710](../src/rendering-techniques/combined/CombinedRenderer.cpp#L710)).
    `PrimitiveFactory::BakeIslandTerrainBrickmap` produces a `BrickmapData`
    in the GPU-ready std430 layout; uploaded once per bake via staging.
  - **Foliage placement** — CPU, on terrain bake or grid-dim change
    ([CombinedRenderer.cpp:756](../src/rendering-techniques/combined/CombinedRenderer.cpp#L756)).
    Walks every (gx, gy), finds the topmost solid voxel of that terrain
    column, emits one `GpuInstance`.
  - **Shadow brickmap topology** — CPU, on terrain bake or instance set
    change ([CombinedRenderer.cpp:847](../src/rendering-techniques/combined/CombinedRenderer.cpp#L847)).
    Static-pool bits (terrain) baked here; dynamic pool starts at zero.
  - **Shadow brickmap dynamic bits** — GPU, every frame (pass 2).
  - **Foliage volume + asset bitmask** — GPU, every frame (pass 1) — but
    the asset is procedurally deterministic, so this is recomputing the
    same content every frame.

### 1.6 Today's instance counts

  - Default foliage grid: 64 × 64 = 4096 instances ceiling, less in
    practice (sea-level filter rejects underwater columns;
    [CombinedRenderer.cpp:792-796](../src/rendering-techniques/combined/CombinedRenderer.cpp#L792-L796)).
  - Inspector clamp: 128 (= 16 384 ceiling /
    [CombinedRenderer.cpp:963](../src/rendering-techniques/combined/CombinedRenderer.cpp#L963)).
  - Terrain: configurable, default 1024×1024×128 voxels (`m_terrain_size`,
    `m_terrain_max_height`).

---

## 2. What we know about today's perf

Performance is described as "useable but not acceptable" for the target
scene. We don't have a per-pass GPU-time breakdown, so the diagnostic
work below is partly inference from architectural shape.

What is observable today:

  - **Three trace passes write `gl_FragDepth` and use `discard`**
    (terrain at [combined_terrain_trace.frag:309, 360](../shaders/combined_terrain_trace.frag#L309)
    and foliage at [combined_foliage_trace.frag:128, 178](../shaders/combined_foliage_trace.frag#L128)),
    so early-Z is disabled in both. Foliage cube AABBs overlap; every
    overlapping pixel pays full DDA + shadow trace cost.
  - **Foliage AABBs are still untightened.** `pc.aabbMin = vec3(0)`,
    `pc.aabbMax = vec3(16, 32, 16)` at
    [CombinedRenderer.cpp:606-609](../src/rendering-techniques/combined/CombinedRenderer.cpp#L606-L609).
    The OPTIMIZATION.md §3.1 finding still applies verbatim: painted
    bounds are ~14% of AABB volume for the grass tuft asset.
  - **Foliage Generate runs every frame** even though it's a deterministic
    procedural function of the asset's parameters. ~16 K dispatches/frame
    (`numXWords × sizeY/4 × sizeZ·frameCount/4` = `1×8×32 = 256` workgroups
    of 1×4×4) for content that doesn't change.
  - **Shadow Foliage Write does atomic-OR work proportional to (instance
    count × bricks-per-instance × 512 threads)**. At 4 K instances each
    touching ~3³≈27 bricks under yaw worst case (likely 4–8 in practice),
    that's 16–32 K workgroups × 512 = 8–16 M atomic-OR-attempts per
    frame. Most of those threads early-out on the asset-bitmask test; the
    cost we *can't* skip is the 8 K–32 K shadow-pool word reads + writes.
  - **Default `maxIterations = 256`, `maxShadowBrickSteps = 256`**
    ([CombinedRenderer.cpp:142-143](../src/rendering-techniques/combined/CombinedRenderer.cpp#L142-L143)).
    Both are safety caps; OPTIMIZATION.md §2 found that lowering them
    proportionally cut trace time for the foliage-only technique. The
    same observation is likely directional for the combined technique
    but at smaller magnitude (the shadow brickmap's empty-brick skipping
    means the average shadow ray walks fewer outer steps to begin with).

What we **do not** know without instrumentation:

  - Per-pass GPU time. Foliage Generate vs Shadow Foliage Write vs
    Terrain Trace vs Foliage Trace.
  - Inside Terrain Trace, time spent in primary DDA vs shadow trace.
  - Inside Foliage Trace, time spent on miss-fragments (cube AABB
    pixels that DDA-out of the volume without hitting) vs hit-fragments
    + shadow.
  - Average shadow-trace outer-step count for a typical scene frame.

§4.0 below lists the specific instrumentation that should land first.

---

## 3. Which old advice still applies

### 3.1 From [OPTIMIZATION.md](OPTIMIZATION.md)

| § | Recommendation                                | Status today | Why |
|---|-----------------------------------------------|--------------|-----|
| 3.1 | Tighten foliage AABB to painted bounds      | **STILL APPLIES** | `pc.aabbMin/Max` still the full 16×32×16 box. The painted volume is roughly 10×10×15. Same ~5–10× inner-DDA-work reduction the original audit estimated; the estimate transfers because the foliage trace shader's inner-DDA loop is structurally unchanged. |
| 3.2 | Lower substrate shadow `kMaxSteps`          | **OBSOLETE in mechanism, partial in spirit** | substrate.glsl is gone; the new `traceShadowWorld` has a configurable `maxShadowBrickSteps` (= 256) instead. Lowering it from 256 to 96 still cuts the worst-case outer walk; see §4.2. |
| 3.3 | Lower inner-DDA cap default                 | **STILL APPLIES** | `maxIterations = 256` is still aggressive. Combined with §3.1 (smaller AABB → shorter natural traversal), 24–48 is plenty. |
| 3.4 | GPU timestamps per pass                     | **STILL APPLIES, MORE URGENT** | The combined technique has 5 passes, not 3. Same instrumentation; bigger payoff because there are more candidates to attribute time to. |
| 4.1 | Depth pre-pass for early-Z recovery         | **STILL APPLIES, MORE URGENT** | Terrain trace + foliage trace both `discard` + write `gl_FragDepth`. Foliage cubes overlap each other AND overlap terrain pixels. A depth pre-pass is now buying early-Z for *two* expensive shaders, not one. |
| 4.2 | Coarse occupancy header in foliage volume   | **PARTIALLY APPLIES** | The asset-bitmask SSBO is already a per-voxel occupancy bit (used by Shadow Foliage Write). It is *not* used by the foliage trace shader's inner DDA — that still does `texelFetch(volume_sampler, …).r`. Wiring the existing bitmask into the inner-DDA path would skip empty voxels for free. (See §5.2.) |
| 4.3 | Per-instance LOD on shadow tracing          | **APPLIES, lower priority** | Helpful when shadow-trace cost is meaningful weight; the empty-brick skipping in `traceShadowWorld` already cuts most shadow rays short. Worth measuring before doing. |

### 3.2 From [instanced.md](instanced.md)

| § | Recommendation                          | Status today | Why |
|---|-----------------------------------------|--------------|-----|
| 2.2 | GPU culling + draw-indirect           | **NOT YET — wrong scale** | At 4 K instances, vertex-shader cost is negligible. Becomes load-bearing past ~100 K. |
| 2.3 | LOD by projected pixel size           | **NOT YET — wrong scale** | Same. Distant blades at 4 K density don't dominate. Worth revisiting at 100 K+. |
| 2.5 | Quantize per-instance state           | **NOT YET — wrong scale** | 4 K × 48 B = 192 KB. No memory pressure. |
| 2.6 | GPU instance generation               | **NOT YET — wrong scale** | CPU placement runs once per terrain bake, not per frame. |
| 2.7 | Multiple-cloud descriptor bug         | **NOT APPLICABLE** | CombinedRenderer has exactly one cloud; the bug doesn't bite. |
| 3   | Multi-species (bindless, etc.)        | **OUT OF SCOPE** for short-term perf. |
| 4   | "Generate pass runs every frame"      | **STILL APPLIES** | Same flaw, same fix (dirty-flag the dispatch). See §4.5. |

### 3.3 From [VISION.md](VISION.md)

  - §3 ("the world-grid occupancy substrate") — **largely realized**.
    The shadow brickmap is the substrate; terrain and foliage both query
    it through `traceShadowWorld`; static + dynamic pools mediate
    contributions on different cadences.
  - The substrate has not yet absorbed §3.6's downstream consumers
    (cross-pillar AO, atmospheric fog, irradiance volume). Those are
    feature work, not perf work, and out of scope here.
  - §5.3 ("GPU-driven everything") — partially realized for the foliage
    side (Shadow Foliage Write is GPU-driven); not yet for the static
    side (terrain bake, foliage placement, shadow brickmap topology
    are all CPU). Not a short-term perf concern at current scale.

---

## 4. Recommended Tier-1 sequence

These are short-term, low-architectural-risk changes that should each
move the perf needle without requiring new render-graph concepts.
Roughly in priority order; each step is gated on the previous one not
already having put us in budget.

### 4.0 GPU timestamps per pass — instrument first

**Effort: ~half a day. Impact on perf: zero. Impact on diagnostic
capability: indispensable.**

Same as OPTIMIZATION.md §3.4. The combined technique has five passes —
without per-pass timestamps, every subsequent change is guesswork. A
`vkCmdWriteTimestamp` pair around each pass + an ImGui readout
distinguishing *Foliage Generate / Shadow Foliage Write / Sky / Terrain
Trace / Foliage Trace* is the foundation everything else gets measured
against.

Specifically wire timestamps so we can answer:

  - Is the per-frame Foliage Generate above-noise, or is the cost
    swamped by the rasterization passes? (If above noise: §4.5 is a
    quick win.)
  - Is Shadow Foliage Write expensive enough to be worth caching across
    frames where animation hasn't progressed an integer step?
  - In Terrain Trace, what fraction of full-screen pixels actually hit
    terrain vs `discard` to sky? (If most are sky, the fullscreen quad
    is doing a lot of nothing — see §4.4.)
  - In Foliage Trace, what's the avg inner-DDA iteration count
    (re-using the existing `total_iters` debug field)?

### 4.1 Tighten the foliage per-draw AABB to painted bounds

**Effort: ~1 day, mostly plumbing. Estimated impact: ~5–10× reduction
in foliage-trace inner-DDA work**, per the original OPTIMIZATION.md §3.1
analysis. Same asset, same shader, same applicability.

The painted bounds for the grass tuft asset are roughly
`(3, 11, 0)..(13, 21, 15)` in asset-local voxels. Setting
`pc.aabbMin/aabbMax` to those bounds gives:

  - Fewer rasterized fragments (AABB silhouette ↓ ~5×),
  - Shorter inner-DDA traversal on miss fragments (~2× fewer steps).

Two ways to propagate the bounds:

  1. **Constant per asset** — bake the painted bounds into the asset
     descriptor at generation time and pass them through the
     `FoliageTracePC` push constant (which already carries `aabbMin`/
     `aabbMax`). This is the right v1 approach: the grass tuft's
     painted bounds are a known function of `kNumBlades`, `ringR`, and
     blade-height range in
     [instanced_voxel_generate.comp](../shaders/instanced_voxel_generate.comp).
  2. **GPU reduction** — a small one-time compute pass over the volume
     image after generation, written to a scalar SSBO. More general but
     overkill for v1.

Note: tightening the AABB also tightens the *vertex shader's*
clip-space bounding box, so off-screen culling improves too — a free
side effect that compounds at higher instance counts.

### 4.2 Lower the iteration caps with margin to spare

**Effort: ~1 hour. Estimated impact: small but free, large if the caps
are currently being hit on hot fragments.**

Three caps to drop:

  - `m_max_iterations` (foliage inner-DDA + terrain primary outer
    cap; default 256 →
    [CombinedRenderer.cpp:142](../src/rendering-techniques/combined/CombinedRenderer.cpp#L142)):
    after §4.1, foliage inner-DDA's natural traversal is ~17 voxels,
    so a cap of **48** is comfortable headroom. Terrain outer cap can
    stay higher (terrain DDA can walk hundreds of bricks across a
    1024-wide island) — split the two values.
  - `maxShadowBrickSteps` (default 256 →
    [CombinedRenderer.cpp:143](../src/rendering-techniques/combined/CombinedRenderer.cpp#L143)):
    256 outer brick steps × 8 voxels/step × 0.0254 m/voxel ≈ 51.8 m of
    shadow ray. Foliage shadows past ~5 m are visually imperceptible;
    drop to **96** (≈19 m) for foliage shadows specifically. Terrain
    shadows under tall geometry want longer rays; a per-pass override
    is reasonable.
  - The world-units `kShadowMaxDist = 64.0` constant in the trace
    shaders ([combined_terrain_trace.frag:339](../shaders/combined_terrain_trace.frag#L339)
    and [combined_foliage_trace.frag:159](../shaders/combined_foliage_trace.frag#L159)):
    same logic as `maxShadowBrickSteps` — 64 m is far past where
    self-foliage shadows matter. **8 m** for foliage; **48 m** for
    terrain.

### 4.3 Depth pre-pass for early-Z recovery

**Effort: ~2–3 days. Estimated impact: ~2–3× on overdraw-dominated
regions.** Same idea as OPTIMIZATION.md §4.1, but the gain compounds
across two expensive shaders now.

Both `combined_terrain_trace.frag` and `combined_foliage_trace.frag`
use `discard` and write `gl_FragDepth`, which disables early-Z.
Concretely:

  - **Terrain trace** is fullscreen — every screen pixel reaches the
    shader before depth-test resolves anything. Pixels that end up
    being foliage (drawn after) still pay full terrain-trace cost
    first.
  - **Foliage trace** has overlapping cube AABBs. Two grass tufts
    behind each other from the camera both do their full inner-DDA
    + shadow trace.

A two-pass split for foliage:

  1. **Foliage depth pre-pass**: rasterize the cube AABBs, run inner
     DDA, on hit write `gl_FragDepth` and discard color; on miss
     `discard`. No lighting, no shadow.
  2. **Foliage color pass**: same rasterization, depth-test `EQUAL`.
     Shading + shadow trace happens once per pixel.

Sequencing matters: doing this **after** §4.1 (AABB tightening) makes
each cube pixel more likely to be a hit and the early-Z gate cleaner
(less wasted depth-prepass work on miss fragments).

For terrain, a depth pre-pass is more invasive (the existing primary
DDA already produces depth as a side effect). One reasonable shape:
have the terrain trace pass write a "terrain depth" image as an
additional color attachment in pass 4, and the foliage trace pass
sample it as a depth-equal test source. But this duplicates the
existing depth attachment's role, so the simpler answer is probably
to keep terrain unchanged and just split foliage.

### 4.4 Skip the fullscreen terrain trace where the island doesn't project

**Effort: ~1–2 days. Estimated impact: depends on framing; potentially
2–4× when the camera frames the horizon (much of the screen is sky).**

Today the terrain trace is a fullscreen quad. The island fits in a
known world-AABB (`±halfExtents` in
[combined_terrain_trace.frag:298](../shaders/combined_terrain_trace.frag#L298)).
Pixels whose ray-AABB-intersection misses cost a vertex stage, a
full enter/exit test, and an early `return h` — but they're still
rasterized and *still cost the FrameUbo bind, the descriptor walk,
and the bare interpolant pipeline*.

Options:

  1. **Cheap:** rasterize a screen-aligned quad sized to the
     island's NDC bounding box instead of fullscreen, computed once
     per frame from the eight world-AABB corners. Pixels outside
     that quad never enter the terrain shader; sky pre-pass already
     painted them. ~3-line CPU change.
  2. **More precise:** rasterize the actual world-AABB cube under
     the camera transform (back-face culled, one draw of 36 verts).
     Same primitive shape as foliage AABBs; the existing terrain
     primary DDA is the inner work. Pays a small overdraw cost on
     near-camera framings (front + back faces both rasterize) for a
     much tighter screen footprint on far framings.

Option (1) probably has 80% of the win for 20% of the effort.

### 4.5 Gate the Foliage Generate pass on an actual change

**Effort: ~30 min. Estimated impact: small per-frame but free, and
removes one unknown from the timing picture.**

`instanced_voxel_generate.comp` is a deterministic function of the
asset's parameters (`kNumBlades`, ring radius, sway amplitudes — all
hard-coded constants). The compute pass re-runs every frame at
[CombinedRenderer.cpp:310](../src/rendering-techniques/combined/CombinedRenderer.cpp#L310),
producing byte-identical output every time.

The fix is a "dirty" flag set when the asset's parameters change
(today: never, after authoring lands: rarely). In v1 the flag can
just be `false` after the first run.

This is the same v1 simplification that
[InstancedVoxelTechnique.cpp:382-386](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L382-L386)
flagged but didn't fix. Now there are two reasons it's worth fixing:
(a) the cost is still nonzero and we don't know its magnitude until
§4.0 lands, and (b) it's a precondition for §4.6.

### 4.6 Wire the asset bitmask into the foliage trace inner-DDA

**Effort: ~1 day. Estimated impact: bounded by §4.1 — likely small once
AABBs are tight, but the change is cheap and additive.**

The asset bitmask
([instanced_voxel_generate.comp](../shaders/instanced_voxel_generate.comp)
binding 1) already encodes "is voxel `(x,y,z)` solid in frame `f`" as
one bit. Today the foliage trace shader ignores it and re-derives the
same information via a per-voxel `texelFetch(volume_sampler, …).r`.

If the inner-DDA loop reads the bitmask first and only does the volume
fetch on hit (to get the *material index* for shading), the per-step
cost on miss voxels drops from one R8 texelFetch + comparison to one
R32 word read + bit test + comparison — and on stride-friendly
walks, one R32 read covers up to 32 voxels' worth of "is anything
solid here" lookups (caller pre-checks the word, then walks bits).

This is the same trick OPTIMIZATION.md §4.2 suggested (a "coarse
occupancy header") but at finer granularity. The infrastructure is
already in place.

---

## 5. Recommended Tier-2 (only if Tier-1 is insufficient)

After Tier-1, re-measure. If still over budget:

### 5.1 Cache the Shadow Foliage Write across frames where the
animation phase is integer-stable

**Effort: ~2 days. Impact: depends on §4.0's measurement.**

The dynamic pool is rewritten every frame, but the *content* of the
write only changes when at least one instance's `instanceFrame`
([shadow_foliage_write.comp:91-96](../shaders/shadow_foliage_write.comp#L91-L96))
crosses an integer boundary. At `m_animation_speed = 0.5` and
`frameCount = 8`, that happens at most once every 0.25 s per instance
— most frames are no-ops if we detect that.

A simpler version: only re-run Shadow Foliage Write on frames where
the *minimum* `time + animOffset` floor crossed an integer for *any*
instance. Conservative; can be tightened later.

### 5.2 Per-instance shadow-trace LOD by distance

**Effort: ~1 day.** Same as OPTIMIZATION.md §4.3. Skip the shadow
trace for foliage instances whose projected footprint is below a
threshold (so distant grass renders as directly-lit-only). Visually
equivalent to no shadow on far blades, which is barely noticeable.

The hook is at
[combined_foliage_trace.frag:149](../shaders/combined_foliage_trace.frag#L149)
right before the `traceShadowWorld` call.

### 5.3 Coarse depth pyramid for occlusion culling foliage

**Effort: ~3–4 days.** A min/max depth pyramid built from the terrain
depth output, sampled in a foliage-cull compute pass that emits a
visible-instance buffer. At 4 K instances this is over-engineered;
becomes attractive only past ~100 K, at which point it's the right
thing per [instanced.md §2.2](instanced.md). Listed for completeness;
**not** the next move.

### 5.4 Move foliage placement and shadow brickmap topology to GPU compute

**Effort: ~1 week.** Today the topology is built on CPU on terrain
bake or grid-dim change. At 4 K instances on a 1024² island, the
bake is < 100 ms (one-shot, not per frame). At 1 M instances on a
streamed 8K² island this dominates startup.

Not a short-term concern. Listed because [VISION.md §5.3](VISION.md)
flags it and it's the natural pairing with §5.3 above.

---

## 6. What's NOT the next move

A few things from the older docs that look directionally right but are
the wrong ROI for short-term work:

  - **GPU culling / draw-indirect plumbing** ([instanced.md §2.2](instanced.md)) —
    4 K instances are not the bottleneck. A vkCmdDrawIndirect refactor
    pays off at 100 K+ instance scale; until then, it's infrastructure
    work that doesn't move the perf needle.
  - **Bindless multi-species** ([instanced.md §3](instanced.md)) — one
    species today. Pure feature work; no perf implication.
  - **Brickmap streaming** ([VISION.md §2.1](VISION.md)) — the 1024²
    island fits in VRAM with room to spare. Streaming becomes load-
    bearing at the next island size step (8K²+).
  - **Quantizing per-instance state** ([instanced.md §2.5](instanced.md))
    — 192 KB at 4 K instances. Don't.

---

## 7. Recommended order

In strict priority, with each step gated on the previous one not
already having put the technique in budget:

  1. **§4.0 GPU timestamps** — instrument first, always. (~half day.)
  2. **§4.5 Gate Foliage Generate on a dirty flag** — cheap, removes
     one unknown. (~30 min.)
  3. **§4.2 Lower iteration caps** — single-line shader changes after
     timestamps confirm the caps are touching hot fragments.
     (~1 hour + visual QA.)
  4. **§4.1 Tighten foliage AABB** — almost certainly the biggest
     single win after instrumentation. (~1 day.)
  5. **Stop and re-measure.** Steps 1–4 should comfortably move the
     trace passes into single-digit milliseconds at 4 K instances.
  6. **§4.3 Foliage depth pre-pass** — if foliage trace is still
     dominant after §4.1. (~2–3 days.)
  7. **§4.4 Skip terrain trace where the island doesn't project** —
     if terrain trace is dominant. (~1–2 days.)
  8. **§4.6 Bitmask-driven inner DDA** — additive, do alongside §4.1.
     (~1 day.)
  9. **Tier-2** as needed.

Steps 1–5 are roughly **3 working days** of focused work for what
should be a comfortable framerate at the current scene scale.

---

## 8. Open questions

  - **Concrete frame-time numbers.** "Useable but not acceptable" needs
    a concrete budget. Standard target is 16 ms (60 Hz); split across
    the five passes that's ~3 ms/pass at peak load. §4.0 makes this
    measurable.
  - **MSAA.** `caps.msaaSamples` is wired through
    ([CombinedRenderer.cpp:166-167](../src/rendering-techniques/combined/CombinedRenderer.cpp#L166-L167)).
    With `discard` + `gl_FragDepth` and thin foliage silhouettes,
    sample-rate shading can multiply fragment cost by sample count.
    Worth confirming whether it's 1× or 4× under typical config, same
    as [OPTIMIZATION.md §7](OPTIMIZATION.md).
  - **Terrain primary trace cost** at 1024² volume. The fullscreen DDA
    walks the brickmap top grid until it hits a populated brick or
    exits the AABB. Sky pixels exit fast; ground pixels typically hit
    within a handful of bricks. But wide-frame views of the horizon
    walk the long axis of the volume — we don't know how many outer
    steps that takes on average until §4.0 lands.
  - **Painted-bounds drift under animation.** §4.1's "constant per
    asset" approach assumes the painted bounds don't shift under
    sway. The grass tuft's wind sway is bounded by `kWindGain * 1.3 ≈
    1.6 voxels` of tip displacement
    ([instanced_voxel_generate.comp:53](../shaders/instanced_voxel_generate.comp#L53)),
    which expands the painted bounds by ±2 voxels horizontally. Bake
    that overhead into the constant.

---

## 9. Reading order

  1. This document — current architecture and short-term plan.
  2. [VISION.md §3](VISION.md) — the substrate concept, now realized as
     the shadow brickmap.
  3. [OPTIMIZATION.md](OPTIMIZATION.md) §3.1, §3.4, §4.1 — the still-
     applicable parts. The §3.2 substrate-trace discussion is obsolete;
     the rest of the doc reads as background.
  4. [instanced.md](instanced.md) — primarily the data-path overview
     in §1; the scaling analyses in §2–§3 are out of scope for short-
     term work but useful when instance counts grow.
  5. The code, starting with
     [CombinedRenderer.cpp](../src/rendering-techniques/combined/CombinedRenderer.cpp),
     [combined_terrain_trace.frag](../shaders/combined_terrain_trace.frag),
     [combined_foliage_trace.frag](../shaders/combined_foliage_trace.frag),
     and the shadow trace include
     [shadow_trace.glsl](../shaders/include/shadow_trace.glsl).
