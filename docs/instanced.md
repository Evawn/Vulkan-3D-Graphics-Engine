# InstancedVoxelTechnique — How It Works & Where It Will Break

This document explains how `InstancedVoxelTechnique` currently renders large
quantities of grass-blade instances and audits how far that design will scale
along two axes the project cares about:

  1. **Instance count** — thousands today, target millions.
  2. **Asset variety** — one species today, target dozens (grass, trees,
     flowers, rocks…) co-existing in one scene.

References use markdown links so you can jump to the exact code. All file
paths are relative to the repo root.

---

## 1. How it works today

### 1.1 The data path

The technique participates in the standard producer/consumer split that the
rest of the renderer uses:

  - **Producer** — a single `SceneNode` carrying a `Component` of type
    `InstanceCloud` ([Scene.h:38](../src/rendering/Scene.h#L38)). The
    component holds an `AssetID` for the voxel volume, a `BufferHandle` for
    the per-instance SSBO, and the `instanceCount` plus a per-instance AABB.
  - **Extractor** — `SceneExtractor::Visit` reads the component each frame
    and emits one `RenderItem` of type `InstancedVoxelMesh` per cloud
    ([SceneExtractor.cpp:58-75](../src/rendering/SceneExtractor.cpp#L58-L75)).
  - **Consumer** — the trace pass declares
    `AcceptsItemTypes({ RenderItemType::InstancedVoxelMesh })` and iterates
    `pctx.scene->Get(...)` to draw each item
    ([InstancedVoxelTechnique.cpp:280](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L280)).

The technique itself owns the asset and the cloud at v1: `RegisterPasses`
checks `m_volume_asset.valid()`, and on first call seeds both the procedural
animated voxel volume *and* the scene node carrying the component
([InstancedVoxelTechnique.cpp:109-125](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L109-L125)).
That's a v1 simplification — eventually the editor / scene system populates
clouds; the technique becomes a pure consumer.

### 1.2 The three render passes

`RegisterPasses` registers three passes, in the order the graph executes them:

  1. **Generate (compute)**
     ([InstancedVoxelTechnique.cpp:193-223](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L193-L223))
     — runs `instanced_voxel_generate.comp` to procedurally paint
     `frameCount × size.x × size.y × size.z` voxels into the asset's 3D
     image. Frames are stored as Z-slabs: frame `f` of voxel `(x,y,z)` lives
     at `(x, y, z + f*size.z)`. Currently runs *every frame* — the comment
     in `OnPostCompile`
     ([InstancedVoxelTechnique.cpp:382-386](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L382-L386))
     explains this is a v1 simplification (1024 dispatches × cheap shader is
     cheaper than the ergonomics of disabling/re-enabling the pass).
  2. **Sky pre-pass** — fullscreen gradient + sun disk so non-cube pixels
     show a sky rather than clear color. `LoadOp::Clear`. The trace pass that
     follows uses `LoadOp::Load` so its cube draws overlay the sky.
  3. **Trace (graphics)** — the actual instanced draw. One
     `vkCmdDraw(36, item.instanceCount, 0, item.firstInstance)` per
     `InstancedVoxelMesh` item
     ([InstancedVoxelTechnique.cpp:354-365](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L354-L365)).

### 1.3 The bounding-volume rasterization trick

This is the core idea of the technique and the reason a single draw can paint
millions of complex voxel volumes. There are no vertex or index buffers:

  - The vertex shader reads `gl_VertexIndex` (0..35) into a hard-coded 36-vert
    cube table and `gl_InstanceIndex` into the per-instance SSBO
    ([instanced_voxel.vert:55-74, 81-103](../shaders/instanced_voxel.vert#L55-L103)).
  - It scales the unit cube by the per-instance AABB
    (`pc.aabbMin..pc.aabbMax`), then applies the per-instance TRS (scale →
    quat-rotate → translate), then the per-cloud `cloudWorld`, then the
    camera `viewProj`.
  - The fragment shader runs once per *covered pixel* of that AABB and
    DDA-marches inside the volume in instance-local space
    ([instanced_voxel.frag:158-180](../shaders/instanced_voxel.frag#L158-L180)).
    The march samples the volume image as `texelFetch(volume_sampler,
    (x, y, z + frameIdx*size.z), 0)`. Misses `discard` so the sky pre-pass
    shows through.

The cost model per *pixel* is:

  - one inverse rotation of the world ray into instance-local space (cheap —
    `cloudWorld^-T` is a 3×3 transpose under the orthonormal-cloud
    assumption, and the per-instance rotation is a quaternion conjugate),
  - up to `m_max_iterations = 96` voxel steps, each a single `texelFetch`,
  - on hit, one palette fetch, optional corner-AO (cheap), and Lambertian
    shading.

### 1.4 Per-instance state

The SSBO entry, `GpuInstance`, is 48 bytes
([InstancedVoxelTechnique.cpp:24-32](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L24-L32)):

```
vec3  position;     float scale;       // 16 B
vec4  rotation;                        // 16 B (quaternion)
float animOffset;
float _pad0;        // reserved for future speciesIndex (bindless multi-species)
float _pad1;
float _pad2;                           // 16 B
```

`animOffset` is a per-instance phase added to the global `time` UBO field
before integer-flooring into a frame index; that's how a single voxel asset
plays at thousands of phases simultaneously. Yaw is quantized to
`{0°, 90°, 180°, 270°}` on the CPU side
([InstancedVoxelTechnique.cpp:401-420](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L401-L420))
because non-axis-aligned rotations force the DDA to march diagonally through
partial voxels and the visual cost isn't worth it for grass.

### 1.5 Frame state and bindings

Two UBOs and one SSBO + two combined-image-samplers
([InstancedVoxelTechnique.cpp:237-250](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L237-L250)):

| binding | type             | contents                                |
| ------- | ---------------- | --------------------------------------- |
| 0       | storage buffer   | per-instance SSBO (48 B × N)            |
| 1       | sampled 3D image | the *single* voxel volume               |
| 2       | sampled 2D image | palette                                 |
| 3       | uniform buffer   | volume meta (size, frameCount)          |
| 4       | uniform buffer   | per-frame state (camera, sun, time, …)  |

A 96-byte push-constant block carries the per-draw `cloudWorld` and the AABB
([InstancedVoxelTechnique.cpp:67-73](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L67-L73)).

### 1.6 Lifecycle

  - **Graph build** allocates the persistent SSBO sized to
    `m_grid_dim^2 * 48 B`, uploads CPU-generated instance data via
    `graph.UploadBufferData` (staging).
  - **Per frame**: `RecordCallback` writes the per-frame UBO from camera /
    `SceneLighting` / `SkyDescription`, then iterates render items and emits
    one push-constant + draw per item.
  - **`m_grid_dim` change** flips `m_pending_grid_rebuild`, then `Reload`
    fires an `AppEventType::RebuildGraph` so the SSBO re-allocates at the
    new size.

---

## 2. Scaling to thousands → millions of instances

### 2.1 What works at scale

  - **Single-draw model is the right shape.** `vkCmdDraw(36, N, …)` plus
    SSBO addressing is exactly how every modern foliage system does it.
    Going from 4K to 4M is no API change — just a bigger SSBO and more
    vertex-shader invocations.
  - **48-byte instance state is reasonable.** 1M instances ≈ 48 MB. 10M
    ≈ 480 MB. That's the right order of magnitude; you can compress further
    (see §2.5) if memory becomes the bottleneck.
  - **`gl_InstanceIndex + firstInstance` already lets multiple draws share
    one SSBO** — the architectural primitive for "one mega-buffer, draw
    sub-ranges" is in place.
  - **Frame-as-Z-slab animation cost is independent of N.** Animation is
    bytes in a static image; one sample per fragment regardless of instance
    count.

### 2.2 The first wall: no culling

This is the biggest gap and the first thing that will fail under load.

Every frame, the trace pass issues `vkCmdDraw(36, instanceCount, …)` with
*every* instance in the SSBO. The rasterizer culls by clip-space, so
off-screen instances cost a vertex-shader invocation but no fragment. That's
fine to ~100K instances. At 1M instances:

  - 36M vertex-shader invocations per frame just to clip 90 % of them away.
  - Worse: nothing culls *inside* the frustum either. A blade behind a hill
    still rasterizes its full AABB, and overdraw of overlapping cubes from a
    grazing camera angle is unbounded.

**What's needed.** A GPU-side cull pass:

  - Compute pass reads the per-cloud SSBO + camera frustum + (eventually) a
    coarse depth pyramid, writes an indirect draw buffer
    (`VkDrawIndirectCommand`) plus a compacted "visible instance" SSBO.
  - Trace pass switches from `vkCmdDraw` to `vkCmdDrawIndirect` /
    `vkCmdDrawIndirectCount`.
  - Engine doesn't have any `DrawIndirect` plumbing yet — the VWrap layer
    has no `CmdDrawIndirect` and the graph has no draw-indirect resource
    state — so this is a real bit of new infrastructure, not a config flip.

A typical rule of thumb: cull → 4× to 20× speedup at 1M+ instances depending
on camera framing. Skipping it isn't optional at the target scale.

### 2.3 The second wall: no LOD

A blade 200 m from the camera does the same 96-iteration DDA per fragment as
a blade 1 m away. With foliage at million-instance density, distant blades
dominate fragment cost.

**What's needed.** Two-tier minimum:

  1. **Screen-space size threshold.** During cull, compute the projected
     pixel footprint; below ~4 px, swap the instance into a
     billboard/impostor draw or just don't draw it.
  2. **DDA early-out by distance.** Scale `frame.maxIterations` per fragment
     by depth or projected size — a 2-pixel blade rarely needs 96 steps.

A future third tier is impostor atlases (pre-rendered N-direction views of
each species) but that's a different code path.

### 2.4 The third wall: one fragment-shader invocation per cube pixel

Bounding-volume rasterization is generous about fragment counts. A 32-tall
voxel scaled to a 0.4-unit blade, viewed up close, can cover ~5K fragments
that all DDA. At 1M instances camera-near, you're easily talking 100M+
fragment-shader invocations. The DDA itself is fine; the *texelFetch
bandwidth* is the concern.

The volume texture is a 3D `R8_UINT`. At `16×32×16×8` slabs that's 65 KB,
fits in L2 forever, so this is fine for the *current* asset. With dozens of
species (see §3) the working set blows up — see §3.4.

### 2.5 Memory & bandwidth at the 1 M – 10 M edge

  - **SSBO size.** 48 B × 10 M = 480 MB. Painful. Two tactics:
    - Quantize: 16-bit half for position relative to chunk origin, 8-bit
      quaternion via smallest-three encoding, 8-bit scale. Total ~16 B,
      same density at 3× compression.
    - Tile: store instances in 64×64 m chunks each with their own SSBO and
      a chunk-local origin. Wins double — smaller per-instance state
      *and* enables coarse chunk frustum culling on the CPU before any GPU
      work.
  - **Animation bandwidth.** Per fragment, `vFrameIdx` is per-instance flat
    so quads in a derivative neighborhood often diverge — texture cache
    works in your favor only when neighboring blades happen to be on the
    same frame. At high density this is fine because each blade is small
    and most quads are *within* a blade.

### 2.6 CPU-side instance generation

`RebuildInstanceData` builds the instance vector on the CPU and uploads via
staging
([InstancedVoxelTechnique.cpp:388-430](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L388-L430)).
For 10 M instances, that's a 480 MB host allocation and a 480 MB staging
copy. Acceptable for "rebuild on grid-dim change" (rare). Not acceptable
for "user paints grass with a brush" (every stroke). The fix is generation
on GPU — a compute pass writing the SSBO from a height map + density map +
species mask. Not needed for v1; needed before the editor lands.

### 2.7 Multiple clouds: actually broken today

Per §1, the trace pass iterates render items and uses each item's
`firstInstance` / `instanceCount` for the draw, but the *descriptor* at
binding 0 is hard-bound to `m_instance_buffer` — the technique's own
single-cloud buffer
([InstancedVoxelTechnique.cpp:244](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L244)).
`SceneExtractor::Visit` does set `item.instanceBuffer` from the component
([SceneExtractor.cpp:67](../src/rendering/SceneExtractor.cpp#L67)), but the
record callback never rebinds. So:

> If a second `InstanceCloud` component appears in the scene, it draws into
> the *first* cloud's SSBO with the second cloud's `firstInstance` offset.

Three ways to fix:

  1. **Concatenate.** All clouds upload into one shared SSBO; each item's
     `firstInstance` is a real offset into it. Cheapest; matches the
     existing "one buffer, sub-ranges" comment in `RenderItem.h`. Requires
     a registry to track offsets.
  2. **Per-item descriptor.** Bind a different SSBO per draw via dynamic
     descriptor sets / push descriptors / per-item descriptor set lookup.
     More flexibility (clouds can be sized independently), more bind
     traffic.
  3. **Bindless instance buffers.** Index into an array of SSBOs by a
     per-draw push-constant slot. Same descriptor-indexing infra you need
     for multi-species (§3.1).

(1) is the right v1 answer. (3) folds naturally into the multi-species
work below.

---

## 3. Scaling to dozens of asset types

This is where the technique needs the most rework. The current pipeline
binds *exactly one* voxel volume image at descriptor binding 1, hard-coded
in `RegisterPasses`
([InstancedVoxelTechnique.cpp:245](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L245)).
Every grass blade in the world reads the same `volume_sampler`. That has to
go.

### 3.1 Bindless descriptor indexing

The right shape for "dozens of species, picked per-instance":

  - Bind a `uniform usampler3D volumes[MAX_SPECIES]` array at binding 1
    (`VK_EXT_descriptor_indexing` /
    `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT` in 1.2 core).
  - Bind a `VolumeMeta volumesMeta[MAX_SPECIES]` array (or one SSBO of
    metas indexed by species) since `meta.size` and `frameCount` differ per
    species.
  - Per-instance, read `inst._pad0` (now `speciesIndex`) and select with
    `volumes[nonuniformEXT(speciesIndex)]`.

Engine prerequisites that are missing:

  - VWrap doesn't enable any descriptor-indexing features today (no
    `VkPhysicalDeviceDescriptorIndexingFeatures` setup). Search confirmed
    zero references to `descriptor_indexing` / `bindless` anywhere in
    `src/` or `lib/VWrap/`.
  - `BindingTable` has no `BindGraphSampledImageArray` helper — it'd need
    one, plus partial-bind / variable-count support.
  - The `AssetRegistry` would need a way to enumerate live voxel-volume
    `ImageHandle`s in a stable order so the descriptor array slots map to
    species IDs deterministically across graph rebuilds.

### 3.2 The cheaper-but-uglier alternative: one draw per species

If bindless is too heavy a lift, the natural fallback is:

  - One scene node (`InstanceCloud`) per species.
  - One render item per cloud.
  - Trace pass record loop swaps the descriptor (image binding 1) before
    each draw.
  - Push-constant `cloudWorld + aabbMin + aabbMax` already changes per
    draw; adding "this draw's volume + meta UBO" is a descriptor-set swap
    (or a push descriptor), not a pipeline rebind.

Tradeoffs vs §3.1:

  - **Pros**: zero new descriptor-indexing infra, works on any GPU.
  - **Cons**: N draws × per-draw descriptor swap. Fine at "dozens"; it's
    not the millions-of-draws regime that bindless solves. Loses the
    "one mega-cull-buffer drawing all foliage" model — each species needs
    its own cull pass and indirect buffer.

For the user's stated target ("on the order of dozens" of species),
**this is probably the right call** as the v2 step, then upgrade to
bindless in v3 when GPU-side culling lands and per-draw overhead starts to
matter.

### 3.3 Animation timing per species

`m_animation_speed` is a single technique-wide tunable
([InstancedVoxelTechnique.h:97](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.h#L97))
fed into `frame.time` ([InstancedVoxelTechnique.cpp:349](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L349)).
Different species want different rates (grass ≠ trees ≠ flowers) and
possibly different `frameCount`s. Two changes needed:

  1. Per-species animation speed and `frameCount` carried in the per-species
     metadata (the SSBO of `VolumeMeta` records in §3.1).
  2. The shader's `int frame_i = int(floor(mod(t, fc)))` needs `fc` from
     that per-species metadata, not the per-frame UBO. Today
     `frame.frameCount` is a single global.

### 3.4 Memory budget for the asset library

A 16×32×16×8 grass blade is 65 KB. Twenty-four species at this size:
~1.5 MB total — trivially fits in L2.

Larger assets (a 64-cell tree volume × 8 frames at R8 = 2 MB; × 24 species
= 48 MB) start to exceed L2 (typically 4–32 MB on consumer GPUs) and
become DRAM-bound during DDA. At that point the per-fragment iteration
cap becomes a real perf knob, not a quality knob — keep it modest.

Also: the volume image format being `R8_UINT` (256 materials per asset)
is asset-local — *each* asset has its own palette. Right now the technique
binds one global palette. In a multi-species world, the palette either
becomes per-species (bound alongside the volume) or unifies into a
global atlas indexed by `(species × 256 + matIdx)`. The latter is much
simpler and the existing palette resource scales fine — palette atlases of
8K×4 RGBA are trivial.

### 3.5 Pipeline state per species

A worry I checked and *don't* think bites: do different foliage types need
different rasterization state? For voxel-based foliage, no — every species
is opaque, back-face cull, depth test/write. Single graphics pipeline is
fine for the entire foliage system.

If a non-voxel species ever wants alpha-tested geometry (e.g. a 2D
billboard species sneaks in), it gets its own technique, not its own
pipeline within this one.

### 3.6 The `_pad0` design hint

The instance-struct comment "reserved for future speciesIndex (bindless
multi-species)"
([InstancedVoxelTechnique.cpp:30](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L30))
shows the data shape was deliberately picked so the per-instance state can
absorb a species index without changing layout or breaking std430. That's
correct — when §3.1 lands, no other systems break.

---

## 4. Other gaps worth listing

These don't touch the headline "millions × dozens" question but will surface
once you start pushing the technique:

  - **Shadows.** `frame.shadowsEnabled` is plumbed through the UBO but the
    fragment shader never casts a shadow ray. The hook is there; the
    implementation isn't.
  - **Non-uniform cloud transforms.** `cloudWorldInv = transpose(mat3)` in
    the fragment shader assumes the cloud world is rotation-only / uniform
    scale. The header documents this. Anything that puts an `InstanceCloud`
    under a non-uniformly-scaled parent will subtly skew the trace
    direction. Validate at component level or pass a proper inverse matrix
    in the push constant.
  - **Float precision at world scale.** `position` is a `vec3` of absolute
    instance-space coordinates within the cloud's local frame, then
    multiplied by `cloudWorld`. Fine if clouds are ≤ ~1 km wide. Beyond
    that, do per-chunk relative offsets (this folds neatly into §2.5).
  - **`m_grid_dim` UI clamp at 128** — that's 16 K instances ceiling from
    the editor today
    ([InstancedVoxelTechnique.cpp:462](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L462)).
    Fine for development; just remember to lift it once the cull pass
    lands.
  - **Generate pass runs every frame.** Cheap *now*. With 24 species each
    doing this, that's 24 × 1024 dispatches/frame for static data. Move
    each species's generation behind a "dirty" flag set by the asset
    registry when the procedural definition changes.

---

## 5. Verdict

**Today, for one species at ≲16 K instances**, the technique is well-shaped
and demonstrates the right primitives: data-driven RenderItems, per-instance
SSBO, AABB rasterization, Z-slab animation. It is a clean v1.

**To reach thousands → millions of one species**, the dominant work is GPU
culling (frustum + screen-size LOD) feeding `vkCmdDrawIndirect`. The
single-draw `vkCmdDraw(36, N, …)` shape stays; the engine grows new
draw-indirect plumbing. Concatenate-into-one-SSBO is a small
prerequisite that also fixes the latent multi-cloud bug in §2.7.

**To reach dozens of species**, the dominant work is descriptor indexing
*or* per-species draws. The data layout already has a slot
(`GpuInstance._pad0`) for a per-instance species index, but neither the
descriptor-set layout, the BindingTable, nor the AssetRegistry expose the
needed bindless surface today — that's where the new plumbing lives.

In short: the technique's *render-pass shape* is at the right altitude for
the target — the gaps are the **engine layers around it** (cull pass,
draw-indirect, descriptor indexing, per-species metadata SSBO), not in the
shaders or the technique class itself.
