# Unified Lighting — World-Grid Occupancy Substrate

A near-term spec for fixing inter-instance shadows in the
`InstancedVoxelTechnique`, designed so the v1 doubles as the seed of
the shared lighting substrate that [VISION.md §3](VISION.md) commits
the engine to. Read VISION.md first for the big-picture argument and
the single-voxel-pitch commitment that makes this work; this document
is what we actually build.

---

## 1. The problem

Today the engine has two shadow paths that don't compose:

  - **Brickmap (terrain) self-shadows** by casting a real secondary
    DDA ray from the primary hit toward the sun, walking the same
    two-level brickmap it just primary-traced
    ([brickmap_palette_trace.frag:308-319](../shaders/brickmap_palette_trace.frag#L308-L319)).
    Voxel-perfect, no resolution loss, scales by ray length.
  - **Instanced foliage shadows** are sampled from a separate depth
    image rendered under the sun's view-proj. The shadow pass
    rasterizes every instance's AABB, runs the same per-instance DDA
    the primary pass uses, and writes `gl_FragDepth` from the actual
    voxel surface hit
    ([InstancedVoxelTechnique.cpp:267-318](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L267-L318)),
    so the shadow silhouette is blade-shaped not cube-shaped. The
    trace pass samples it as `sampler2DShadow` with hardware PCF
    ([instanced_voxel.frag:101-123](../shaders/instanced_voxel.frag#L101-L123)).

Three things break:

  1. **Cross-pillar shadowing.** The foliage shadow pass declares
     `AcceptsItemTypes({ RenderItemType::InstancedVoxelMesh })`
     ([InstancedVoxelTechnique.cpp:268](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L268))
     so terrain isn't in the shadow map. The brickmap's shadow ray
     walks only the brickmap, so foliage isn't visible to terrain
     receivers either. A blade casts no shadow on the ground beneath
     it, and the ground casts none on a blade growing in its lee.
  2. **Inter-instance precision at scale.** A 2K shadow map covering
     the foliage extent gives texels much larger than a world voxel
     (the world voxel is the same fine pitch the terrain uses; see
     [VISION.md §1.1](VISION.md)). Once blade voxels project sub-
     texel, multiple blades compress to one shadow sample and the
     result blobs. The shadow pass cost also scales with
     `instanceCount × cube fragments`, so larger maps don't help
     linearly.
  3. **The asymmetry itself is the problem.** Adding water (or any
     fourth pillar) as a shadow caster/receiver requires either re-
     implementing the brickmap path against new geometry or
     extending the shadow pass to accept new RenderItem types.
     Neither scales.

The fix is to retire the foliage shadow map and replace it with a
**world-space occupancy index** that all pillars can write into and
shadow rays can walk. Per VISION.md §3.2, this index is not a new
data structure built alongside the brickmap — it *is* the brickmap,
broadened to accept contributions from other pillars. The static
contribution is already there. The work in this document is mostly
about adding the foliage contribution and the unified query layer
that consumes them.

---

## 2. Constraints we're accepting

The substrate works iff foliage geometry aligns to the integer world
voxel grid. Because all pillars share a voxel pitch
([VISION.md §1.1](VISION.md)), this is a much narrower constraint
than it would be in a multi-pitch engine — there's no asset rescaling
to reconcile, just position quantization.

The concrete sub-requirements all contradict the current
`InstancedVoxelTechnique`'s defaults, but only modestly:

  - **Per-instance position must be quantized to integer multiples
    of the world voxel size.** Today positions are continuous with
    ±0.05 sub-voxel jitter
    ([InstancedVoxelTechnique.cpp:478-481](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L478-L481)).
    Drop the jitter; quantize to the integer grid offset. Visual
    variety comes from per-instance assets and per-instance yaw, not
    from sub-voxel placement.
  - **Per-instance scale stays at 1.** Today scale is randomized
    around a small base value
    ([InstancedVoxelTechnique.cpp:486-487](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L486-L487))
    derived from `m_grid_spacing / m_volume_size.x`. Because asset
    voxels *are* world voxels (VISION.md §1.1), the only correct
    scale is 1: each asset voxel cell occupies exactly one world
    voxel cell. Authored variants (a tall blade, a short blade) ship
    as separate assets, not as a per-instance scale knob.
  - **Per-instance rotation stays yaw-quantized.** Yaw is already
    4-way quantized
    ([InstancedVoxelTechnique.cpp:471, 489-490](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L471))
    which is sufficient: each yaw is a permutation+sign-flip of the
    instance-local axes onto world axes. Pitch/roll stays at zero.

The cloud-level world transform (the SceneNode hierarchy in
[Scene.h](../src/rendering/Scene.h)) needs to be a permutation +
integer-translation in world voxels for clouds that participate.
This is a runtime invariant the technique should validate, not an
authoring freedom we expose.

A non-obvious consequence: the current asset's nominal 16×32×16
voxel volume only makes sense if those 16×32×16 are *world voxels at
the canonical pitch*. If the project's voxel pitch ends up smaller
than the implicit one the current grass blade was authored at, the
asset will need to be re-bake at the canonical pitch (and the
content gets denser). This is a content-side adjustment, not an
engine-side one — but it's the kind of thing easy to forget when
voxel pitch finally gets pinned down.

---

## 3. The data structure

### 3.1 Per-brick contributions in the brickmap

The substrate brickmap (which is the static brickmap, broadened —
see [VISION.md §3.2](VISION.md)) carries multiple contribution layers
per brick:

  - **Static contribution.** The terrain brickmap's existing brick
    payload (palette indices in 8³ packed words). For occupancy
    queries this collapses to "voxel non-zero" — the test
    [`brickVoxelMaterial(...) != 0u`](../shaders/brickmap_palette_trace.frag#L82-L93)
    that the brickmap shader's existing `isSolidAt` already wraps.
    No change to terrain's storage; we add a thin occupancy adapter
    on top.
  - **Foliage contribution.** A *per-brick instance overlap list*:
    the set of instance IDs whose AABB touches this brick. An
    instance's "occupancy" at a given world voxel is a function of
    the instance's quantized transform, the asset's per-frame
    bitmask, and the global animation time — so we cannot pre-OR
    instances into a single bitmask without losing per-instance
    phase. The list is the index; the per-frame bitmask asset is the
    payload.
  - **Dynamic contribution.** (Future, water and particles.) A
    per-frame bitmask layer the dynamic pillar's voxelization step
    writes; out of v1 scope, mentioned only to confirm the data
    shape supports it.

The brickmap's existing 8³ brick layout
([Brickmap.h:15-25](../src/rendering/voxel/Brickmap.h#L15-L25))
already gives us the spatial index for free; the work is adding the
foliage-overlap-list layer alongside the existing brick pool.

### 3.2 Per-instance per-frame bitmask asset

For each animated voxel asset (today there is one; eventually one
per species), generate a `frameCount × size.x × size.y × size.z` bit
array. Bit `(f, x, y, z)` is 1 iff the asset's frame `f` has a non-
empty voxel at `(x, y, z)`. This is a *new* asset slot alongside the
existing `R8_UINT` volume image; the volume image holds material
indices for the renderer, the bitmask holds occupancy for the
substrate. Keeping them separate avoids forcing the substrate to
fetch full bytes when it only wants a bit.

Because instances are *small* — a grass blade is at most a few dozen
voxels in any axis at the canonical world-voxel pitch — the bitmask
asset is tiny. A 16×32×16 grass-blade asset across 8 frames is
65 536 bits = 8 KB. A 64³ tree asset across 16 frames at 1 bit/voxel
is 512 KB. A library of two dozen species is comfortably in the
single-MB range total. Animated-occupancy storage is *not* the
expensive part.

The bitmask is built at the same time as the volume image. The
existing
[`instanced_voxel_generate.comp`](../shaders/instanced_voxel_generate.comp)
that procedurally writes the volume can write the bitmask in the
same dispatch; for `.vox`-imported assets the bitmask is built CPU-
side during import and uploaded once.

### 3.3 Per-brick instance overlap list

For each brick that any instance touches, store a list of instance
indices that contribute to it. CSR-like layout:

  - `brick_offsets[brick_index]` → starting offset into a flat
    `brick_instances[]` array.
  - `brick_offsets[brick_index + 1] - brick_offsets[brick_index]` =
    count of overlapping instances.
  - `brick_instances[i]` = instance index into the existing per-cloud
    SSBO.

The build step is straightforward when an instance is created or
moved:

  1. Compute the instance's world-space AABB from its quantized
     transform plus asset size in world voxels.
  2. Iterate the world bricks the AABB overlaps. Because instances
     are small (typically a few voxels per axis) and bricks are 8
     voxels per axis, an instance touches one to a small handful of
     bricks.
  3. Append the instance index to each touched brick's list.

Deletion reverses it. For v1 we accept the constraint that builds
happen in batch (one full rebuild per `RebuildInstanceData`);
incremental updates are a v2 concern. The existing
[`RebuildInstanceData`](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L458-L500)
already does a one-shot CPU pass; we extend it to also produce the
per-brick lists.

### 3.4 Sizing and budget at v1

For the current 64×64-instance grass cloud at canonical voxel pitch:

  - Each blade is small (a few world voxels per axis) and touches
    one or two bricks.
  - 4096 instances × ~2 bricks each = ~8000 instance-list entries.
  - At 4 bytes per entry, the foliage overlap layer is ~32 KB.
  - The brickmap's static layer for the same footprint is whatever
    the terrain decides, independent of foliage.

At v1 we cover the foliage cloud's footprint only. The substrate
brickmap can be small (a few thousand bricks, sub-MB total) because
that's all the foliage occupies. As the static pillar takes on
larger scenes the substrate inherits its streaming concerns
([VISION.md §3.5](VISION.md)) — but v1 doesn't have to solve
streaming.

### 3.5 What v1 deliberately is not

  - **Not streamed.** The substrate at v1 is sized to the active
    foliage cloud. When the static pillar starts streaming, the
    substrate's overlap-list layer streams alongside it (same brick
    coordinates, same load/evict cadence). That work lands when the
    static pillar takes it on, not in this milestone.
  - **Not GPU-built.** `RebuildInstanceData` is the only entry point
    for instance changes today, and it runs CPU-side already
    ([InstancedVoxelTechnique.cpp:458-500](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L458-L500)).
    Extending it to emit per-brick lists is a CPU change. GPU build
    becomes necessary once interactive instance edits land in the
    editor; out of v1 scope.
  - **Not multi-species-aware.** v1 has one asset, so per-instance
    bitmask lookups don't need to dispatch on species. The data
    shape supports it (per-instance index → per-asset bitmask), but
    multi-species lands with the per-species draw work in
    [instanced.md §3](instanced.md), not here.

---

## 4. The shadow query

Let `traceShadowWorld(origin, dir, maxDist)` be the substrate-side
function that replaces the foliage shadow-map sample at
[instanced_voxel.frag:165](../shaders/instanced_voxel.frag#L165) (and
eventually replaces the brickmap's inline secondary trace at
[brickmap_palette_trace.frag:312-319](../shaders/brickmap_palette_trace.frag#L312-L319)
too).

### 4.1 Algorithm

```
Walk the substrate brickmap via DDA from origin toward sun, capped at maxDist.
For each brick the ray enters:
    1. If the brick has any contribution (static-occupied OR foliage
       overlap list non-empty OR dynamic layer non-empty):
         a. Build a per-brick combined occupancy bitmask covering this
            brick's 8³ voxels = 512 bits. Initialize from the static
            contribution (if any). For each overlapping instance, look
            up the asset's bitmask at its current frame, transform from
            instance-local voxel coords to brick-local voxel coords
            using the instance's quantized TRS, and OR into the
            combined mask. (Future: OR the dynamic layer.)
         b. Inner DDA: step voxel-by-voxel through the brick. At each
            voxel, test the combined bitmask. If set → blocked, return 0.
    2. Else step to next brick.
If we exit the substrate AABB or hit maxDist without occlusion → 1.
```

The combined-bitmask step at brick entry is the load-bearing perf
choice. The alternative — per-voxel, walk the instance list and
test each — pays `O(instances_per_brick)` per voxel. Building the
combined mask once at brick entry is `O(instances_per_brick)` per
*brick*; the per-voxel test then costs one bit lookup. For any ray
that crosses ≥2 voxels of a brick (essentially all of them), the
combined approach is strictly faster.

The combined mask is per-thread state (eight `uint64`s, or a
`uvec4` × 4 group, depending on what compiles best). It does not
materialize in memory; it lives in registers for the lifetime of
the brick's inner loop.

### 4.2 What the trace pass shader becomes

The fragment shader's lighting block at
[instanced_voxel.frag:158-169](../shaders/instanced_voxel.frag#L158-L169)
shrinks to:

```
NdotL  = max(0, dot(worldNormal, sunDir));
shadow = traceShadowWorld(hitWorld + worldNormal * bias,
                          sunDir,
                          someMaxDist);
ambient = skyColor * ambientIntensity * ao;
direct  = sunColor * sunIntensity * NdotL * shadow;
```

`sampleSunShadow`
([instanced_voxel.frag:101-123](../shaders/instanced_voxel.frag#L101-L123))
is deleted along with the entire shadow-map pass, the
`shadow_sampler` binding, the `lightViewProj` UBO field, and the
[`InstancedVoxelTechnique.m_shadow_map`](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.h#L79)
image. This is a *net subtraction* from the foliage pillar — the
substrate absorbs that work and the pillar loses an entire pass and
its descriptor surface.

### 4.3 Bias and self-intersection

The same hazard the brickmap shadow ray handles applies here: the
ray starts at the hit voxel's surface and must not re-hit itself.
Two mechanisms compose:

  - **Normal bias.** Offset along the surface normal by a fraction
    of a world voxel before tracing — same approach the brickmap
    uses
    ([brickmap_palette_trace.frag:315](../shaders/brickmap_palette_trace.frag#L315)).
  - **Origin-voxel skip.** Optionally, skip the voxel containing the
    ray origin in the inner test. Most relevant for the foliage
    contribution, where the receiver voxel might also be a caster;
    the static contribution handles its own self-occlusion via bias.

The current `shadowBiasConstant` and `shadowBiasSlope` UBO fields
([InstancedVoxelTechnique.cpp:62-63](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L62-L63))
stay relevant and reused.

---

## 5. Build and update

The substrate has three populate paths, one per pillar:

  - **Terrain (one-time, scene-load → eventually streamed).** The
    static contribution is the existing
    [`BrickmapData`](../src/rendering/voxel/Brickmap.h)'s payload,
    queried via the brickmap shader's existing `isSolidAt`. Because
    the substrate brickmap *is* the terrain brickmap (VISION.md
    §3.2), no copy or re-bake is needed; we add a query adapter on
    top. When streaming arrives, the terrain side stages bricks in
    and out and the substrate's per-brick foliage layer rides
    alongside.
  - **Foliage (on instance create/delete).** Extend
    [`RebuildInstanceData`](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L458-L500)
    to additionally produce the per-brick instance lists. CPU build
    is fine for v1 (one-shot, paired with the existing instance
    upload).
  - **Dynamic (per-frame, future).** A water/particle pillar's
    voxelization compute pass writes its frame's contribution as a
    sibling layer that the query ORs in. Out of v1 scope.

The query interface (`isSolidAtWorld` / `traceShadowWorld`) does not
care which pillars contributed — the per-frame bake produces, the
shadow query consumes.

---

## 6. Implementation milestones

A v1 that fixes the headline bug (inter-instance shadows) should
land in this order. Each milestone is independently shippable; each
takes the previous one's invariants for granted.

### Milestone A — quantize the foliage technique

No substrate work yet. Strip the constraint-violating instance
generation:

  - Drop position jitter
    ([InstancedVoxelTechnique.cpp:479-480](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L479-L480)).
    Quantize positions to the integer world voxel grid.
  - Drop the scale randomization
    ([InstancedVoxelTechnique.cpp:486-487](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L486-L487))
    and fix scale at 1, so per-asset voxels and world voxels agree
    one-to-one.
  - Add a runtime assertion in `RebuildInstanceData` that every
    instance's transform composed with its cloud's world transform
    produces a permutation + integer-translation in world voxels.

Visual check: scene should look subtly less varied but otherwise
identical. The existing shadow map keeps working. **Ship this
alone first** — it's a self-contained content change that's a
prerequisite for everything below, and an easy place to validate
that the canonical voxel pitch decision (VISION.md §1.1) doesn't
introduce surprises.

### Milestone B — per-frame bitmask asset

Add the bitmask layer to
[`AssetRegistry::CreateProceduralAnimatedVoxelVolume`](../src/rendering/AssetRegistry.h)
or a sibling. Either:

  - Re-run the existing generate compute with a different output (a
    storage buffer of bits sized to the asset), or
  - Add a CPU post-pass that derives the bitmask from the staged
    volume bytes after the existing pipeline produces the volume
    image.

The volume image stays unchanged; the bitmask is additive. Validate
correctness by adding a debug visualization mode that reads only
the bitmask and renders it as occupancy.

### Milestone C — substrate brickmap (foliage-only contribution)

Build the substrate's per-brick instance overlap layer as a graph-
managed buffer. Static and dynamic layers stay empty for now: the
brickmap pillar is still self-shadowed via its existing inline
path, and the foliage shadow map is still rendered.

Add the `traceShadowWorld` shader function but consume it only in
a new debug mode the editor can flip on, side-by-side with the
existing shadow-map result. Validate visually that the substrate
produces matching (or better) shadows than the shadow map, and
profile to confirm per-ray cost is in budget at the current
instance density.

### Milestone D — switch foliage shading to the substrate

Flip the trace pass to use `traceShadowWorld` for sun shadow.
Delete the shadow pass, the `m_shadow_map` image, the
`shadow_sampler`, and `lightViewProj` from the foliage UBO. Verify
the foliage technique still has the same or better visual fidelity.

This is the milestone that actually fixes inter-instance shadows.
Inter-instance occlusion is now voxel-perfect: any blade casts on
any other blade through the same DDA query, with no shadow-map
resolution loss.

### Milestone E — fold the static brickmap into the substrate query

Connect the terrain brickmap's existing payload as the substrate's
static contribution. Shadow rays from foliage hits now walk
through terrain bricks; rays from terrain hits walk through
foliage bricks (replacing the brickmap's inline self-trace). At
this point the foliage and terrain pillars use the same shadow
path.

The visual payoff that justifies the refactor cost: foliage now
correctly receives terrain shadow, and terrain receives foliage
shadow. Hill shadows on grass, blade shadows on dirt, both at
voxel precision.

### Out of scope for v1

  - GPU-side rebuild of the instance lists.
  - Streaming the substrate alongside terrain streaming (rides on
    the static pillar's streaming work, whenever that lands —
    [VISION.md §3.5](VISION.md), [§5.1](VISION.md)).
  - Multi-species bitmask dispatch (folds into the multi-species
    work in [instanced.md §3](instanced.md)).
  - Cone-traced AO over the substrate. The substrate's
    `isSolidAtWorld` makes this trivial; we add it after Milestone
    E if AO quality visibly suffers from staying per-pillar.
  - Dynamic pillar (water, particles). The data shape supports it;
    nothing in v1 implements it.

---

## 7. Performance notes and watchpoints

Things to measure when this lands and not before:

  - **Per-fragment shadow-ray cost** vs the current shadow-map
    sample. The shadow map is one PCF lookup per fragment; the
    substrate is a DDA whose length depends on sun angle. For
    grazing sun angles, rays are long; for high noon, short.
    Sampling cost variance across viewing conditions will be
    larger than today.
  - **Instances per brick.** Per-brick instance count drives the
    per-brick OR-into-scratch cost at brick entry. Because instances
    are small and bricks are 8³ voxels, the typical overlap count
    is low (a brick fully blanketed by grass might see ~4-16
    instances each occupying a small chunk of the brick). Watch
    the worst case in case the OR pass becomes the bottleneck on
    extreme densities.
  - **Build cost** when `RebuildInstanceData` runs. CPU traversal
    of instances → per-brick lists is `O(instance_count ×
    bricks_per_instance)`. At 4 K instances × ~2 bricks each, no
    concern. Past ~100 K instances the CPU build starts to take
    real wall-clock time; that's the trigger for moving the build
    to GPU.
  - **Substrate buffer sizes** at scene scales beyond the current
    cloud. The foliage overlap layer is bounded by `instances ×
    bricks_per_instance`; the substrate brickmap proper is bounded
    by terrain extent. Both stay manageable until terrain extent
    grows and streaming becomes load-bearing — which is a static-
    pillar problem the substrate inherits, not something this v1
    needs to solve.

A specific anti-pattern to avoid: do *not* OR every instance's
bitmask into a single world-space bitmask once per frame. That
tempting simplification breaks per-instance animation phasing
(different instances are at different `frameIdx` at the same global
time) and amounts to recomputing the entire foliage occupancy every
frame regardless of camera. The per-brick-list-with-on-demand-OR-at-
brick-entry is the right tradeoff: build cost paid once on instance
change, ORcost paid per *visited* brick during shadow rays only
(work proportional to where shadows actually need to be evaluated,
not to how much foliage exists offscreen).

---

## 8. Forward-looking: this as the substrate

What v1 ships is mechanically just "fix inter-instance shadows in
the foliage technique." The reason it's worth the structure described
above (rather than a smaller patch like "render terrain into the
foliage shadow map") is that the data shapes — per-instance per-
frame bitmask, per-brick instance list, substrate brickmap with
multiple contribution layers, `isSolidAtWorld` query — generalize
cleanly:

  - **Adding a third pillar (water, particles, etc.).** New
    contribution layer in the substrate; the query function ORs it
    in. No change to foliage, no change to terrain.
  - **AO over the substrate.** `cornerAO`'s `isSolidAt` callback
    becomes `isSolidAtWorld`; AO becomes cross-pillar.
    [voxel_ao.glsl](../shaders/include/voxel_ao.glsl) is unchanged;
    the includer just provides a different callback.
  - **GI / sky occlusion.** A coarse irradiance volume aligned with
    the substrate brick grid has its lookup answered by
    `isSolidAtWorld` samples around each grid cell.
  - **Gameplay queries** (collision, "what's under the player"):
    same `isSolidAtWorld`, different consumer.
  - **Wind/disturbance fields.** Sibling per-brick layers; same
    coordinate convention, different payload.
  - **Streaming.** When the static pillar starts streaming bricks
    in and out around the camera, the substrate's foliage overlap
    layer rides on the same brick lifecycle — no new streaming
    machinery, the same brick load/evict events drive both.

The v1 milestones above produce a substrate that doesn't *do* any
of those things yet — but it doesn't have to. The architectural
commit is that the next time we want any of them, we extend the
substrate instead of adding a fourth bespoke per-technique
implementation.

The asymmetry between "brickmap casts a shadow ray, foliage samples
a shadow map" goes away in v1 (Milestone D). The asymmetry between
"lighting, collision, AO, GI, fog all touch occupancy independently"
is what goes away across the next year.
