# Engine Vision

A technical companion to [FEATURE.md](../FEATURE.md). FEATURE.md is the
mood board — a grassy island, swaying foliage, painterly light, sixty
frames a second. This document is the *engineering* side of that picture:
the shape the engine takes if we follow the mood board to its conclusion,
and the architectural commitments that are load-bearing along the way.

It is intentionally not a spec. No data layouts, no shader pseudocode, no
priorities-with-dates. The goal is to record the technical *intent*
behind the project so that any individual decision can be evaluated
against the larger arc.

For where things stand today, see [instanced.md](instanced.md) (deepest
audit of the foliage pillar) and [LIGHTING.md](LIGHTING.md) (concrete
near-term plan for the unified-occupancy substrate this document argues
for).

---

## 1. The shape of the engine

The scene the engine renders is not generic. It is one specific kind of
world: a dense, voxel-built natural environment with stylized lighting,
heavy animation, and authoring-time procedural content. Every
architectural choice should be evaluated against fitness for *that*
scene, not against fitness as a general-purpose voxel renderer.

This matters because every non-trivial system in the engine has a "be
general" temptation and a "be specific" temptation, and the right
answer is almost always closer to the specific end than feels
comfortable. The engine's value comes from being *good* at this one
scene, not adequate at many.

### 1.1 The single voxel pitch

The most important architectural commitment in this document is that
the engine has *one* voxel pitch, scene-wide. Every cubic centimeter of
the world — terrain, grass, tree leaves, water, particle effects — is
built out of voxels of the same edge length. There is no
"terrain voxel" vs. "foliage voxel" distinction; there are voxels.

This is non-obvious because nothing forces it. The brickmap pillar
parameterizes its voxel size at runtime via `pc.voxelWorldSize`
([brickmap_palette_trace.frag:18-20](../shaders/brickmap_palette_trace.frag#L18-L20))
and the instanced pillar derives its voxel size per-cloud from
`(aabbMax - aabbMin) / size` independently
([instanced_voxel.frag:127](../shaders/instanced_voxel.frag#L127)).
Today nothing makes them agree. The commitment is that they *will*
agree, and every system in the engine — coordinate conventions, asset
formats, the world-grid lighting substrate (§3) — should be designed
on the assumption that voxels are interchangeable across pillars.

Every other choice in this document follows from that.

### 1.2 Three planning anchors

  - **Bespoke pipelines, shared substrates.** The scene's three
    rendering pillars (§2) get dedicated passes whose internal details
    are tuned to the pillar's content and its specific scaling
    challenge. Where they share something — palette, shading formula,
    occupancy query — they share a thin substrate that each pillar
    reads/writes through narrow interfaces, rather than a fat
    framework all three inherit from. The shared `shadeLit` in
    [lighting.glsl](../shaders/include/lighting.glsl) and the shared
    `cornerAO` in [voxel_ao.glsl](../shaders/include/voxel_ao.glsl) are
    the prototype shape; the world-grid occupancy substrate (§3) is
    the next, larger increment of the same pattern.
  - **GPU-driven by default.** Each pillar's scaling story bottoms out
    in GPU-built or GPU-walked structures. The instanced pillar's path
    to a million blades runs through draw-indirect, GPU culling, and
    compute-built instance buffers; the static pillar's path to a
    streamable island runs through GPU brickmap loads; the world-grid
    substrate's path to whole-scene queries runs through compute-built
    indices. Every system that scales benefits from the same posture.
    This is a deliberate orientation, not a series of incidental
    performance fixes.
  - **Authoring is part of the engine.** A foliage engine without a way
    to procedurally grow plants is just a viewer. The plant authoring
    tool (§4) and its data formats are first-class concerns, not
    "we'll figure it out later."

---

## 2. The three rendering pillars

The scene decomposes naturally into three categories of geometry. They
share voxel pitch and shading model; they differ in *what kind of
breadth* dominates each one's design, and the rendering technique each
one uses is the one specifically optimized for its kind of breadth.

### 2.1 Static geometry — the terrain

Currently brickmap-palette voxels rasterized via a fullscreen DDA
tracer ([brickmap-palette/](../src/rendering-techniques/brickmap-palette/)).
The brickmap shape — an 8³ brick pool with a top-level sparse grid and
single-byte palette indices, all in one std430 buffer that uploads with
a single staging copy ([Brickmap.h:31-49](../src/rendering/voxel/Brickmap.h#L31-L49))
— is a good fit for terrain.

**The challenge for this pillar is spatial extent.** A diorama-scale
island, voxelized at the engine's canonical voxel pitch, contains
hundreds of millions to tens of billions of voxels. Most are empty
(sky above ground, rock below crust), so sparse storage matters; the
brickmap's two-level structure already exploits that. But even with
99% empty bricks the data outgrows VRAM at scene scale: an island that
spans 256 m × 256 m × 64 m at a 5 cm voxel pitch is 5120 × 5120 × 1280
voxels = ~33 billion voxels, and even at 1% nominal sparsity this is
hundreds of MB of brick payload before palette and metadata.

The work this pillar will require, in roughly increasing order of
distance from today:

  - **Streaming.** Bricks are loaded into VRAM around the camera and
    evicted when far. The brickmap's flat std430 layout is fine for
    "load everything at once" but doesn't naturally support partial
    residency; the next-generation layout has to.
  - **Procedural source.** `.vox` imports are fine for the prototype.
    The target is procedurally-generated terrain (heightmaps, biome
    masks, hand-sculpted overlays) that builds the brickmap as part
    of the asset pipeline. The `BrickmapData` struct is the right
    abstraction boundary: any producer that emits one feeds the same
    renderer.
  - **GPU-side build.** Once the source is procedural, the build
    itself can move to the GPU — a compute pass that emits brick
    payloads from a sampling kernel — which both removes a CPU
    bottleneck and is the natural place for runtime regeneration if
    we ever want it.

The pillar's existing strengths are worth keeping:

  - The brickmap already casts secondary shadow rays against itself
    via the same two-level DDA
    ([brickmap_palette_trace.frag:308-319](../shaders/brickmap_palette_trace.frag#L308-L319)).
    Self-shadowing on terrain just works. This is the model the other
    pillars should look like to the lighting substrate (§3).
  - Sparsity is the right primitive at this pillar's scale. Anything
    that erodes it (dense per-voxel metadata, unconditional payload
    bytes per brick) will hurt long-term.

What this pillar does *not* try to be: it does not try to support
fully dynamic terrain edits inside a frame. "Paint a hill" rebuilds
the brickmap offline and swaps the asset; in-frame edit cost is not a
goal.

### 2.2 Animated instanced geometry — foliage and small objects

Currently grass blades via the
[InstancedVoxelTechnique](../src/rendering-techniques/instanced-voxel/),
documented at length in [instanced.md](instanced.md). The shape — flat
3D voxel volumes per asset, frame-as-Z-slab animation, AABB-rasterization
with inner DDA — is the right primitive for this pillar.

**The challenge for this pillar is instance count.** Each individual
asset is small: a grass blade is a handful of voxels (a few wide, a
few dozen tall at most), at the same voxel pitch as the terrain. A
flower head, a leaf, a pebble — all small. The aesthetic richness
comes not from any one asset's complexity but from having millions of
them in one frame. Per-instance memory is ~tens of bytes; per-instance
*throughput* is what dominates.

The work this pillar will require:

  - **GPU culling and draw-indirect.** Today the technique issues
    `vkCmdDraw(36, instanceCount, …)` against the entire SSBO, and
    rasterization clips off-screen instances at the cost of one
    vertex-shader invocation per off-screen blade. At a million
    instances this is wasteful; at ten million it's fatal. The fix is
    a GPU cull pass emitting a `VkDrawIndirectCommand` plus a
    compacted visible-instance buffer ([instanced.md §2.2](instanced.md)
    has the full audit).
  - **LOD.** Distant blades doing 96-step DDA are bandwidth-bound;
    they need either an early-out by projected pixel size or an
    impostor swap below a threshold ([instanced.md §2.3](instanced.md)).
  - **Multi-species.** One asset today; "dozens" target. The
    [`GpuInstance._pad0`](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L30)
    slot reserved for `speciesIndex` is the data plane; the runtime
    needs descriptor indexing or per-species draws to match
    ([instanced.md §3](instanced.md)).
  - **Per-instance simulation state.** Wind that bends grass,
    footsteps that flatten it, flowers that bloom on a timer — all
    per-instance properties. The 48-byte SSBO entry already carries
    transform + animation phase + reserved padding; the *direction*
    of growth (more per-instance signals) should be planned now
    rather than back-fit.

A specific asymmetry that needs to go: where the brickmap pillar
casts a real DDA shadow ray, this pillar samples a separate shadow
depth image rendered under the sun's view-proj
([InstancedVoxelTechnique.cpp:267-318](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L267-L318)).
That shadow map can't see terrain (the shadow pass only accepts
`InstancedVoxelMesh` items) and degrades to blob shadows at high
blade density (sub-texel blades). The asymmetry — and the inability
of either pillar to shadow the other — is what motivates the
substrate (§3); the near-term plan to retire the foliage shadow map
in favor of a substrate query lives in [LIGHTING.md](LIGHTING.md).

### 2.3 Dynamic geometry — water, particles, and content-specific systems

Not yet implemented. The scene calls for water that displaces, foams,
and catches light differently than terrain or foliage; eventually
particle effects (spray, dust, falling petals) will join it.

**The challenge for this pillar is that there is no single challenge.**
Each kind of dynamic content has its own scaling profile:

  - Water surfaces are large, mostly flat, and updated coherently
    every frame; they look like compute-shader fluid simulation
    feeding a per-frame voxel write.
  - Particle systems are small per-particle, many particles, with
    individual lifecycles; they look like GPU-emitted instance lists
    drawn as voxel point sprites or short voxel trails.
  - Smoke or volumetric effects might never voxelize at all — they
    might be a compute pass writing into a sparse 3D density field
    that a separate ray-marcher samples.

The right architectural posture is therefore not "build *the* dynamic
pillar" but "build a *family* of dynamic techniques as each kind of
content arrives, sharing the substrate but otherwise specialized." A
water technique is a separate technique from a particle technique;
both contribute occupancy to the substrate (§3) on their own
cadences, and beyond that they share little.

What unifies them at the engine level is exactly what unifies the
other two pillars: voxel pitch, shading model, occupancy substrate.
What divides them is everything specific to their content kind, and
that divide is fine — forcing a unified "dynamic pillar" interface
would shape the engine around an abstraction that doesn't pay rent.

A concrete sibling worth tracking: [AnimatedGeometryRenderer](../src/rendering-techniques/animated-geometry/)
already exists as a single-level-DDA technique against a procedurally
generated 3D volume. It's positioned as scaffolding for the foliage
pillar today, but its shape (compute-generated dense volume,
single-level DDA) is closer to a water-style dynamic technique than
to instanced foliage. It may eventually evolve into the first
concrete dynamic-pillar technique rather than be retired.

### 2.4 Why three pipelines, not one

A recurring temptation is to merge two pillars: "foliage volumes are
just small brickmaps, why two paths?" or "water is just a thin
animated brickmap, why a separate technique?"

The reason is that each pillar's *scaling pressure* is fundamentally
different, and the right data structure differs accordingly:

  - **Static** is large-extent, cold-update — wants sparse
    spatial indexing and streaming.
  - **Instanced** is small-asset, hot-instance-count — wants
    per-instance SSBO addressing, indirect draws, and amortized
    asset reuse.
  - **Dynamic** is content-specific — wants compute-direct writes
    on its own cadence, shaped by the content kind.

A unified storage format would have to absorb the worst-case update
cost of every consumer, paid by every consumer. Three storage
formats, each tuned to its scaling profile, is dramatically cheaper
in steady state. The interface that *should* unify is the voxel-pitch
agreement and the lighting/query layer (§3), not the storage layer.

---

## 3. The world-grid occupancy substrate

This is the most consequential cross-pillar architectural commitment
in the document. It is also the one most likely to be re-evaluated as
it meets reality, so the framing here is "why this is the right shape"
rather than a final design.

### 3.1 What's already shared, and what isn't

The engine has been quietly factoring shared substrate for a while.
As of now, three things are already shared across pillars:

  - **The shading formula.** [`shadeLit`](../shaders/include/lighting.glsl)
    is one function consumed by every voxel technique. Albedo, AO,
    NdotL, and shadow factor go in; lit RGB comes out. Adding a new
    pillar that uses this formula is one `#include`.
  - **The AO algorithm.** [`cornerAO`](../shaders/include/voxel_ao.glsl)
    is parameterized over an `isSolidAt(ivec3)` callback the
    includer provides; the algorithm itself is identical across
    techniques.
  - **`SceneLighting` and the per-frame UBO.** Sun direction,
    intensity, sky color, AO strength are scene-wide and shared at
    the data level
    ([SceneLighting.h](../src/rendering/SceneLighting.h)).

Two things are conspicuously *not* shared:

  - **Occupancy queries.** Each shader has its own `isSolidAt`
    definition that knows about its own storage (the brickmap shader
    walks the brickmap; the instanced shader does a `texelFetch`
    against its own per-instance volume image). Neither can ask the
    other.
  - **Shadow casting.** Brickmap casts a real DDA shadow ray;
    instanced rasterizes a shadow map. They don't compose: foliage
    doesn't shadow terrain and vice versa.

The engine has unified the *small* shared abstractions (a function, a
struct) but every *large* shared concept (a spatial index, a query
path) is still per-pillar. The substrate fixes the largest missing
one first.

### 3.2 The substrate is the brickmap

Because all pillars share a voxel pitch (§1.1), the static brickmap
is *already* a world-grid spatial index at the right resolution.
There is no parallel "world grid" structure being introduced
alongside the brickmap; the substrate **is** the brickmap, with two
generalizations:

  - The brick payload broadens from "static voxel material indices"
    to "occupancy contributions from any pillar." The terrain
    palette indices become one contribution; per-brick instance
    lists for foliage become another; per-frame dynamic contributions
    (water, particles) become a third.
  - The brickmap's lifetime broadens from "one immutable scene-wide
    asset" to "a streamed, multi-contributor index updated on the
    cadences each pillar dictates."

This collapse is a real architectural simplification. We don't need
to build a new spatial structure, agree on its coordinates with the
existing brickmap, or keep them in sync. The brickmap *is* the
shared structure; pillars contribute into it.

### 3.3 The query interface

Consumers see one function:

```glsl
bool isSolidAtWorld(ivec3 worldVoxel);
```

…and a sibling that does an entire DDA shadow ray:

```glsl
float traceShadowWorld(vec3 worldOrigin, vec3 worldDir, float maxDist);
```

This is precisely the same shape as the existing `isSolidAt` callback
that `cornerAO` already parameterizes over. Each shader's private
`isSolidAt` becomes a thin wrapper over `isSolidAtWorld` once the
substrate is in place; AO becomes cross-pillar for free.

The shadow query walks the world brickmap once and queries each
contributing pillar at each brick. Static contribution is a direct
brick-payload lookup (today's brickmap inner DDA, unchanged).
Foliage contribution is a per-instance, per-animation-frame bitmask
lookup driven by the brick's overlap list. Dynamic contributions
plug in as additional layers as those pillars come online.

The implementation details — how foliage contributes, how the inner
loop combines contributions efficiently, how per-instance animation
phase is handled — are in [LIGHTING.md](LIGHTING.md).

### 3.4 What this is NOT

To resist scope creep:

  - **Not a unified renderer.** Each pillar still has its own
    rasterization path tuned to its content's scaling pressure. The
    substrate is the *query* surface for lighting, not the *render*
    surface.
  - **Not the storage format for any pillar's renderable data.** Each
    pillar keeps its native renderable representation (brickmap
    payload, per-instance volume images, water buffers); the
    substrate's per-brick metadata is a derived index, additive to
    those.
  - **Not a global mutex.** Pillars contribute on their own cadences
    (terrain at scene-build / streaming-load, foliage at instance-
    create-or-delete, dynamic content per-frame). A central component
    that mediates all writes would reintroduce the bottleneck the
    multi-pillar architecture was built to avoid.

### 3.5 Architectural commitments this imposes

Committing to the substrate locks in a few things:

  - **Voxel pitch is global and frozen at scene-build.** Already a
    commitment from §1.1; reiterated here because the substrate is
    where it bites hardest. Changing voxel pitch later means
    rebuilding every contribution.
  - **World-grid alignment for participating geometry.** Geometry
    that wants to participate must align to the integer voxel grid.
    For terrain, automatic. For foliage, this means quantizing per-
    instance position to integer world voxels and keeping per-
    instance scale at 1 (asset voxels = world voxels by §1.1). Yaw
    is already 4-way-quantized
    ([InstancedVoxelTechnique.cpp:471, 489-490](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L471))
    which is sufficient: each yaw is a permutation+sign-flip of the
    instance-local axes onto world axes. Geometry that cannot align
    (e.g. high-frequency water detail finer than the voxel grid)
    participates approximately or opts out and keeps a private
    shadow path.
  - **Animation phase has to be world-queryable.** For foliage, the
    substrate stores enough information to answer "is voxel X
    occupied *right now*" for animated content whose occupancy
    depends on per-instance phase. The per-instance per-frame
    bitmask scheme in [LIGHTING.md](LIGHTING.md) is one way;
    alternatives exist. What's important is that the *interface*
    exposes "right now" so consumers don't have to know about
    animation internals.
  - **The substrate inherits the static pillar's streaming
    challenge.** At island scale the substrate cannot live in VRAM
    in its entirety any more than the terrain brickmap can. Because
    they're the same structure (§3.2), this is automatic — the
    substrate streams when the brickmap streams. But it does mean
    the substrate's design has to anticipate streaming from the
    start, even if v1 only covers the foliage cloud's footprint.

### 3.6 What the substrate enables beyond shadows

If the brickmap becomes the queryable source of truth for "where is
solid," more than direct lighting comes for free:

  - **Cross-pillar AO.** A short cone of substrate samples around a
    hit point gives consistent AO across pillars. The shadow under a
    tree on grass on terrain becomes one darkening computed over one
    index.
  - **Collision and gameplay queries.** A character or particle's
    "what am I touching" becomes the same query a shadow ray uses.
    Whether or not the engine ever has gameplay, having the
    substrate in place removes a class of "you'd have to redo this
    for physics" objections.
  - **Wind/disturbance fields.** Sibling layers on the same brick
    grid; same coordinate convention, different payload. The
    substrate carries occupancy; sibling fields carry other spatial
    signals on the same voxel-pitch convention.
  - **Future irradiance / sky-occlusion bake.** A coarse irradiance
    volume aligned with the substrate's brick grid has its lookup
    answered by `isSolidAtWorld` samples around each grid cell.
  - **Atmospheric fog with in-scene shafts.** Fog density derived
    from substrate ray-march length gives free volumetric shafts
    without screen-space approximations.

These extensions should not drive the v1 design — but the v1 design
should not foreclose them.

---

## 4. Authoring and content

### 4.1 The plant tool

FEATURE.md describes a first-party plant authoring tool. Technically,
this means at least three concerns:

  - **A representation for plants** that is more structured than "a
    3D voxel volume + animation frames." A plant has parts (stem,
    leaves, petals), animation parameters (sway frequency, anchor
    points), species traits (color palette ranges, height
    distribution). The flat voxel volume is the *output* of the
    authoring tool, not its internal representation.
  - **An export pipeline** that bakes the structured representation
    into the engine's runtime asset format (the same flat voxel
    volumes the instanced pillar consumes today via
    [`AssetRegistry::CreateProceduralAnimatedVoxelVolume`](../src/rendering/AssetRegistry.h)).
    Because every asset voxel is a world voxel (§1.1), the export
    bakes at the engine's canonical pitch — there's no per-asset
    "resolution" knob that drifts the asset away from the world
    grid.
  - **A procedural-variation hook** so that one authored species
    produces many distinct instance volumes — different flower
    colors, different blade lengths, different animation phases at
    bake time rather than runtime.

This is consequential because it pushes the engine toward a
*content-many, asset-few* model. A scene with one authored "flower
species" might pull from dozens of generated voxel volumes (color
variants, size variants), each used by thousands of instances. The
multi-species scaling work in [instanced.md §3](instanced.md) is the
runtime side of that; the plant tool is the authoring side. The
[`GpuInstance._pad0`](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L30)
slot reserved for `speciesIndex` is the data plane that connects them.

### 4.2 Where the authoring tool lives

Two reasonable options:

  - **Inside the engine** as another panel/mode. Pros: live preview,
    reuses the renderer for visualization. Cons: bloats the runtime
    with editor-only code; "engine" and "editor" become hard to
    decouple.
  - **A separate offline tool** that emits asset files the engine
    consumes. Pros: clean runtime, lets the editor have its own UX
    that isn't constrained by an in-game viewport. Cons: live
    preview requires either embedding a renderer in the editor or a
    hot-reload pipeline.

The current renderer already has a Dear ImGui editor surface, so the
in-engine path has momentum. The right answer probably lands in the
middle: editor-side schemas and procedural definitions live in editor
code, but they bake into runtime asset files (not consumed live by
the runtime), so the runtime stays clean.

### 4.3 The asset registry as a coordination point

The existing [`AssetRegistry`](../src/rendering/AssetRegistry.h)
carries voxel volumes and meshes today. For the multi-species,
multi-pillar future, it becomes the coordination point for:

  - Voxel volumes (instanced pillar).
  - Brickmaps (static pillar — currently lives in the technique
    itself as a `BrickmapData`; would migrate up).
  - Palette atlases (shared — currently each technique constructs
    its own
    [`PaletteResource`](../src/rendering/voxel/PaletteResource.h)).
  - Eventually: plant-species procedural definitions (for the
    editor), water-surface presets, particle-system templates, wind
    fields.

The risk is that AssetRegistry becomes a god-object. The mitigation
is that "registry" should only mean *handle issuance and lifetime
management*; the actual data shapes live in pillar-specific
subsystems the registry knows handles for but does not own behavior
on.

---

## 5. Engine layer implications

A few engine-level questions follow from the pillar + substrate
model. None of them are urgent, but the project's posture toward each
will shape what the next year of work looks like.

### 5.1 The render graph

The current graph
([RenderGraph.h](../src/rendering/RenderGraph.h)) is a declarative
DAG with fine-grained `ResourceUsage` semantics, transient/persistent
lifetimes, queue affinity hints, and Kahn's-algorithm topological
ordering with stable tie-breaking. The shape is healthy. Three
specific gaps the substrate and pillar work will surface:

  - **Conditional pass execution.** A pass that only runs when its
    inputs change is not a first-class concept today. The instanced
    pillar's procedural-volume generate pass already wants this and
    works around it with a comment-confessed v1 simplification
    ([InstancedVoxelTechnique.cpp:447-455](../src/rendering-techniques/instanced-voxel/InstancedVoxelTechnique.cpp#L447-L455)).
    The substrate bake (re-running on instance create/delete or on
    terrain stream-in) wants the same thing.
  - **Cross-frame persistence with invalidation.**
    `Lifetime::Persistent` already handles "survive viewport resize";
    we'll need a sibling concept of "survive until inputs change."
  - **Streaming-friendly resource model.** The static pillar's path
    to a streamable island wants resources whose contents change
    region-by-region without reallocation. The graph today
    re-allocates persistent buffers on `Compile()`; a streamed
    brickmap wants a stable buffer with sub-region updates.

### 5.2 The scene model

The current
[`Scene` + `SceneNode` + `Component`](../src/rendering/Scene.h)
model is a hierarchical tree with per-node components, each carrying
an `AssetID`. The
[`SceneExtractor`](../src/rendering/SceneExtractor.cpp) is a per-
frame producer that walks the tree and emits typed `RenderItem`s
into a `RenderScene`; techniques are pure consumers. This separation
is clean and worth preserving.

What may need attention as the editor matures:

  - Components that can attach to multiple pillars (e.g. a "wind
    volume" that affects foliage shaders in the instanced pillar
    *and* writes into a dynamic-disturbance scalar field for the
    water pillar).
  - A non-hierarchical query model: "every node within this AABB"
    for streaming, culling, and editor selection. The hierarchy
    stays for transforms; the queries layer on top.

### 5.3 GPU-driven everything

Each pillar's scaling story bottoms out in GPU-built or GPU-walked
structures:

  - Static streaming wants brickmap loads/evictions managed by the
    GPU.
  - Instanced needs cull+LOD compute and `CmdDrawIndirectCount`
    ([instanced.md §2.2](instanced.md)).
  - The substrate's bake — at scale, when instance counts and
    streamed terrain regions both scale up — wants to run in compute.

VWrap has no `CmdDrawIndirect` and the graph has no draw-indirect
resource state today, so this is real new infrastructure rather than
a config flip. Once it exists, it generalizes across all of the
above.

### 5.4 Post-processing has somewhere to grow

The
[`PostProcessChain`](../src/rendering/post-process/PostProcessChain.h)
already supports ordered fullscreen effects on the resolved scene
image (bloom and lens flare exist). The substrate work doesn't
disturb this layer, but two things will eventually matter:

  - **No tonemap pass** today. Once the lighting model has more
    dynamic range (sun disk, sky gradient, future emissives) tonemap
    stops being optional.
  - **Atmospheric fog** is a natural consumer of the substrate — fog
    density derived from substrate ray-march length gives free in-
    scene light shafts. Not a v1 ask; flagged so we don't reach for
    screen-space fog by default when the better tool is already
    there.

---

## 6. Non-goals

A useful list to keep honest:

  - **Generality.** Not a general voxel renderer. If a feature only
    helps non-natural scenes (cities, dungeons, sci-fi), it doesn't
    belong unless it incidentally helps the natural scene.
  - **Realism.** The lighting model can stay stylized. We do not need
    physically-based BRDFs, multi-bounce GI, or volumetric scattering
    unless the scene visibly suffers without them. The substrate
    (§3) makes some of these *cheap* to add later if we change our
    mind, but the default is "no."
  - **Real-time editing of static content.** Terrain is built
    offline. Foliage instance clouds rebuild on parameter change but
    not interactively at 60Hz.
  - **Networking, audio, gameplay.** Out of scope. The engine is a
    diorama renderer with authoring; that's enough.

---

## 7. Open questions

Things that aren't decided and probably shouldn't be decided yet:

  - **The exact voxel pitch.** §1.1 commits to a single global
    pitch; the specific number isn't pinned down here. It interacts
    with shadow quality, query cost, asset detail, terrain memory,
    and instance count. Best resolved empirically when one of those
    starts visibly suffering.
  - **The streaming model for the static pillar.** Brick-LRU around
    the camera is the obvious starting point; chunk-based loading
    keyed to a coarser spatial index is a refinement; both want the
    graph's streaming-resource concept (§5.1) before they're worth
    designing in detail.
  - **Multi-species in the foliage pillar.** Bindless descriptor
    indexing or per-species draws? [instanced.md §3](instanced.md)
    walks through the tradeoffs; the answer probably lands on
    per-species draws first, bindless later.
  - **Plant-tool data model.** A node graph (Houdini-style)? A
    parametric template language? Direct voxel painting? All three
    have project-fit; the call probably comes once the runtime side
    of multi-species is more concrete.
  - **Which dynamic content kind ships first.** Water has the
    strongest aesthetic pull; particles have the simplest scoping.
    Either is a defensible starting point. Each will land as its
    own technique, contributing to the substrate.
  - **`AnimatedGeometryRenderer`'s long-term role.** Scaffolding to
    be retired, or seed of a dynamic-pillar technique? Its single-
    level DDA over a procedural volume is a much closer fit to
    dynamic water than to instanced foliage.

---

## 8. Reading order for new contributors

Someone new to the project should read in this order:

  1. [FEATURE.md](../FEATURE.md) — what we're building.
  2. This document — *why* the engineering shape looks the way it
     does, and the single voxel-pitch commitment.
  3. [instanced.md](instanced.md) — the deepest pillar today, with a
     full audit of where it scales and where it breaks.
  4. [LIGHTING.md](LIGHTING.md) — the next concrete piece of
     architecture being built, and the v1 of the substrate.
  5. The code, starting with `src/rendering/RenderGraph.cpp`,
     `src/rendering/SceneExtractor.cpp`, and the three technique
     directories under `src/rendering-techniques/`.

The repo's individual files are well-commented; the documents above
exist to provide the *why* that comments deliberately don't.
