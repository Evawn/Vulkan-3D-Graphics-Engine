# SCENE-GRAPH.md — First-class scene authoring & extraction

A focused plan for adding a dedicated **scene** layer to the engine: a single
source of truth for what exists in the world (entities, transforms, assets,
camera, lighting, sky), separated from the techniques that *render* that world.
The point is not to bolt a generic scene graph onto the side of the renderer —
it's to draw a clean producer / consumer line so the upcoming foliage workflow
and instanced-animated-geometry technique have a stable interface to plug into.

This document is **descriptive and prescriptive only — no code is written**.
Each section names what to add, why, and roughly how disruptive it is. Tags:

- **[F]** prerequisite for the procedural foliage workflow
- **[I]** prerequisite for instanced animated geometry rendering
- **[E]** editor-facing payoff (inspector, hierarchy panel)
- Effort: **S** = under a day, **M** = a few days, **L** = week+

---

## 0. Top-line summary

After REFACTOR.md, the engine has:

- A **render graph** that owns resources, barriers, descriptors, and pipelines.
- A **technique** abstraction that declares passes and consumes a per-frame
  list of drawable atoms (`RenderScene` of `RenderItem`s).
- A **RenderingSystem** that clears the scene each frame, invokes
  `EmitItems()` on every active technique, and lets the graph hand the scene
  to each pass via `PassContext::scene`.

What's missing is the *producer*. Today, each technique doubles as both producer
(it owns its OBJ file, .vox file, sampler, transform state) and consumer (it
declares passes, records draws). The scene graph fixes this:

- A **Scene** owns world contents — nodes, asset references, camera, lighting, sky.
- A **SceneExtractor** walks the scene each frame and fills the existing
  `RenderScene` with `RenderItem`s.
- An **AssetRegistry** owns asset *data* (vertex buffers, voxel volumes,
  animated voxel textures), addressed by stable IDs that scenes reference.
- Techniques become *pure consumers* — they declare passes, accept item types,
  and record draws. They no longer own geometry, textures, or world state.

After this lands, the layering is:

```
Application
   └─ RenderingSystem
         ├─ Scene                  (NEW: world state)
         │     ├─ SceneNode tree   (transforms + components)
         │     ├─ Camera           (moved from RenderingSystem)
         │     ├─ SceneLighting    (moved from Renderer)
         │     └─ Skybox / sky asset reference
         ├─ AssetRegistry          (NEW: graph-backed asset storage)
         ├─ SceneExtractor         (NEW: Scene → RenderScene each frame)
         ├─ Renderer               (graph + post-process; unchanged shape)
         │     └─ RenderTechnique  (consumer-only; EmitItems() goes away)
         └─ event queue / profiler (unchanged)
```

The win for the foliage and instanced-geometry features is that they slot in
without changing anything above the technique line. A new `InstanceCloud`
component on a node is enough to populate millions of `InstancedVoxelMesh`
items per frame; the foliage editor edits assets in the registry; a "render
foliage" technique just iterates `scene.Get(RenderItemType::InstancedVoxelMesh)`.

---

## 1. What's already in place (do not re-build)

Several pieces of the producer/consumer split already exist. The plan below
*extends* them, not replaces them.

| Existing piece | What it gives us |
|---|---|
| [`RenderItem`](src/rendering/RenderItem.h) — POD drawable atom with handle-based geometry, instance-range, voxel asset reference, transform, AABB | The wire format between scene and technique |
| [`RenderScene`](src/rendering/RenderScene.h) — bucketed-by-type item store, cleared each frame | The frame-local materialized output of extraction |
| `RenderTechnique::EmitItems()` ([RenderTechnique.h:103](src/rendering-techniques/RenderTechnique.h#L103)) | The seam that the SceneExtractor will replace |
| `GraphicsPassBuilder::AcceptsItemTypes(...)` | Per-pass declaration of which item buckets to iterate |
| `Lifetime::Persistent` on `BufferDesc` / `ImageDesc` | Lets the asset registry park asset data in graph-managed buffers/images that survive resize |
| `BindingTable` auto-rebinds on Compile / Resize | Asset images bound through a binding table will rebind correctly when the scene swaps assets |
| `IInspectable` ([Inspectable.h](src/editor/Inspectable.h)) | One polymorphic surface for SceneNode editing in the inspector |

So this plan inherits a strong foundation. Most of the work is on the
*producer side* of an interface that already exists.

---

## 2. The Scene module — what it owns and what it does not

### 2.1 Scene as a small tree, not a deep one  **[F][I]** **(M)**

The scene is a **shallow tree** of `SceneNode`s: tens to low hundreds of nodes
for top-level world structure (terrain chunk, building, prop, light cluster,
foliage patch). It is *not* a node-per-grass-blade tree — that would re-invite
the "millions of nodes" trap. Massively-instanced content lives inside a
single `InstanceCloud` component on a single node, which owns a bulk
per-instance buffer.

A `SceneNode` carries:

- A name (debug + editor display).
- A local transform (TRS — translation, rotation, scale).
- A cached world transform + dirty flag (recomputed on extraction when dirty).
- A list of children (owned by the parent).
- An optional list of **components** — see §2.2.
- Optional visibility / enabled flag — pruned during extraction without
  removing the node itself.

The tree's job is *authoring* — letting the user (and the foliage editor)
group, transform, and toggle world content. Extraction flattens it.

### 2.2 Components are tagged-union data, not virtuals  **[F][I]** **(S)**

Each component is one of a small, fixed set of variants, dispatched by an
enum (`ComponentType`) the same way `RenderItem` is dispatched by
`RenderItemType`. Avoids:

- A `class Component` virtual base + N subclasses (cache-unfriendly, harder to serialize).
- Type erasure via `std::any` (loses static checking, slower).

Initial component set, sized to the planned features:

| Component | Emits (per frame) | Notes |
|---|---|---|
| `MeshComponent` | one `RenderItem::Mesh` | References an `AssetID` for vertex/index buffers |
| `VoxelVolumeComponent` | one `RenderItem::BrickmapVolume` | References an `AssetID` for the volume image |
| `InstanceCloudComponent` | one `RenderItem::InstancedVoxelMesh` with `instanceCount = N` | Owns the per-instance SSBO; references a foliage asset |
| `LightComponent` | does not emit a draw | Mutates `SceneLighting` during extraction (sun direction etc. derive from a node transform when present, otherwise from raw spherical controls) |
| `CameraComponent` | does not emit a draw | The active camera is the one Scene::activeCamera points at |
| `SkyComponent` | (TBD — see §6) | A scene-level sky reference; emission shape depends on §6 |

A node may carry zero or many components. Adding new types later (decals,
particle systems, audio sources) is purely additive — define the variant,
teach the extractor to emit, register an item type if it's drawable.

### 2.3 What the Scene does *not* do  **[T]** (clarification, no work)

Excluded by design, to keep the module honest:

- **No physics, no scripting, no game logic.** This is a render-side scene
  only. The author of this engine builds rendering-focused tools, not a
  general game runtime.
- **No streaming or LOD selection** — that's future work atop the §5 extractor.
- **No serialization yet** — see §10. The scene is in-memory only at v1.
- **No undo/redo or transactional editing** — that's editor concerns.

---

## 3. Asset registry — a real home for "the data"

### 3.1 Why we need it now  **[F][I]** **(M)**

Each existing technique currently owns its own asset loading:

- `MeshRasterizer` parses OBJ in `RegisterPasses`, uploads to a graph-managed
  Lifetime::Persistent buffer ([MeshRasterizer.cpp:148](src/rendering-techniques/mesh-rasterizer/MeshRasterizer.cpp#L148)).
- `BrickmapPaletteRenderer` loads `.vox` and stores it in `m_loaded_vox`,
  re-uploading to a graph image on every Compile.
- `AnimatedGeometryRenderer` will need an animated voxel texture loaded from
  somewhere.
- The future foliage workflow generates voxel textures via compute and needs
  to *publish* them so the renderer can sample them.

These are all the same problem with the same solution: an engine-owned,
content-addressable store of "loaded asset → graph resources that hold its
data." Lifetime is `Persistent` — assets survive resize and graph rebuilds.

### 3.2 What it owns  **[F][I]** **(M)**

`AssetRegistry` is an indirection layer between **asset descriptions** (paths,
generation parameters, type) and **graph handles** that point at the uploaded
data. Initial asset types:

| AssetType | Stored as | Loaded from |
|---|---|---|
| `MeshAsset` | `BufferHandle vertex`, `BufferHandle index`, `uint32 indexCount`, AABB | OBJ file path |
| `Texture2DAsset` | `ImageHandle`, format, extent | image file path |
| `VoxelVolumeAsset` | `ImageHandle` (3D R8_UINT), `volumeSize`, palette | `.vox` file path |
| `AnimatedVoxelAsset` | `ImageHandle` (3D R8_UINT or 2D array), `frameCount` | `.vox` for now; future: foliage-editor compute output |

Public surface:

- `AssetID Load(MeshDesc)`, `AssetID Load(VolumeDesc)`, etc. — idempotent;
  same desc returns the same ID.
- `const MeshAsset& Get(AssetID)` for extraction time.
- `void Reload(AssetID)` — re-runs the loader, swaps buffer contents.
- `void Unload(AssetID)` — refcount-driven; safe to call when no scene
  references the asset.

### 3.3 How it interacts with the render graph  **[F][I]** **(S, after §3.2)**

Two design constraints, both subtle:

1. **Graph rebuilds clear handles.** `RenderGraph::Clear()` bumps the gen
   counter. Persistent buffers/images themselves survive, but their handles
   need to be re-created. The registry needs to redeclare its persistent
   resources during each `RenderingSystem::RebuildGraph`. This mirrors what
   `MeshRasterizer::DeclareGeometryBuffers` already does — the registry just
   centralizes the pattern.

2. **Asset uploads run after Compile.** The graph allocates resources during
   `Compile()`; uploads run via `RenderGraph::UploadBufferData` after that.
   The registry hooks `OnPostCompile` (existing virtual on technique today;
   gets generalized — see §4) to re-upload asset payloads into the new
   buffers. For foliage assets generated by compute, "upload" instead means
   "the compute pass that built me already wrote the data, no host upload
   needed."

### 3.4 Asset *generation* lives outside the registry  **(deferred)**

The foliage editor's compute-driven asset construction is a separate concern
from asset *storage*. Generation will run as one or more compute passes that
write into a registry-owned `AnimatedVoxelAsset` image. The registry just
provides the destination handle and the lifetime guarantee. The generation
pipeline itself is out of scope for this document — see the future
FOLIAGE.md.

---

## 4. RenderTechnique becomes a pure consumer

### 4.1 What goes away from the technique  **[I]** **(M)**

After the scene module lands, the technique loses:

- `EmitItems(scene, ctx)` — the SceneExtractor (§5) does this for every
  technique uniformly.
- All asset-loading code (OBJ parsing, .vox loading, palette uploads). The
  technique requests assets from the registry by ID if it needs to bind one
  globally; otherwise the per-item `voxelAsset` handle on `RenderItem`
  carries the reference.
- Per-item transform composition. The extractor produces world-space
  transforms; the technique just consumes `item.transform`.

What stays:

- `DescribeTargets` — what scene-image stack the technique needs.
- `RegisterPasses` — declarative pass setup, binding table, pipeline state.
- Per-frame record callbacks — bind pipeline + descriptor set, iterate
  `pctx.scene->Get(MyType)`, emit draws.

### 4.2 Three reference techniques after the migration  **[I]** **(S each)**

| Technique | Consumes | Notes |
|---|---|---|
| `MeshRasterizer` | `RenderItemType::Mesh` items | OBJ asset loaded by registry; technique only owns its uniform buffers + the rasterizer pipeline state |
| `BrickmapPaletteRenderer` | `RenderItemType::BrickmapVolume` items | .vox asset loaded by registry; the volume image on each item is bound dynamically per draw (see §4.3) |
| `AnimatedGeometryRenderer` (future-real version) | `RenderItemType::InstancedVoxelMesh` items | Per-instance SSBO sourced from `InstanceCloudComponent`; voxel asset bound per draw |

### 4.3 Per-item resource binding — the awkward case  **[I]** **(M)**

Today, every binding lives in a static `BindingTable` set up at
`RegisterPasses` time. That works for "the technique uses one volume" but
breaks for "the technique iterates N items, each with its own volume image."
The InstancedVoxelMesh path needs this — different foliage species use
different animated assets in the same frame.

Options, in increasing effort and capability:

1. **Texture array / bindless approach.** Bind a single descriptor array
   indexed by per-instance ID. Cleanest at draw time; needs descriptor
   indexing (Vulkan 1.2 extension, very portable now). Recommended target.
2. **Per-pass binding table updates.** Have the pass record callback
   `vkUpdateDescriptorSets` per draw. Easiest to write but stalls; only OK
   for the static brickmap case.
3. **Push descriptors.** Avoid the descriptor-pool churn. Middle ground.

Pick 1 for the InstancedVoxelMesh path. The asset registry already returns
`ImageHandle`s, so collapsing them into a bindless array is a registry-side
detail.

---

## 5. SceneExtractor — the per-frame producer

### 5.1 Replaces the per-technique EmitItems loop  **[F][I]** **(M)**

The current `RenderingSystem::DrawFrame` does:

```
m_scene.Clear()
for each technique: technique->EmitItems(m_scene, ctx)
graph.SetScene(&m_scene)
```

After the scene module:

```
m_scene.Clear()
m_extractor.Extract(*m_world, m_scene)   // walks the SceneNode tree
graph.SetScene(&m_scene)
```

The technique's `EmitItems` virtual is removed. The extractor is the *single*
producer; techniques only consume.

### 5.2 What extraction actually does  **[F][I]** **(M)**

Per frame:

1. **Walk the tree depth-first.** For each node, if dirty, recompute
   `worldTransform = parent.world * node.local`. (No-op on cached entries.)
2. **For each component on the node, dispatch by type:**
   - `MeshComponent`: lookup `AssetID` → `MeshAsset`; emit one `RenderItem::Mesh`.
   - `VoxelVolumeComponent`: emit one `RenderItem::BrickmapVolume`.
   - `InstanceCloudComponent`: ensure the per-instance SSBO is up to date
     (re-upload only if dirty), then emit one `RenderItem::InstancedVoxelMesh`
     with `instanceCount = N` referring to that buffer's range.
   - `LightComponent`: write into `SceneLighting` (light is the rare
     non-drawable component that still affects rendering).
   - `CameraComponent`: if this is the active camera, copy its world
     transform into `Camera`'s view matrix (cameras driven by node transform
     ride along for free with parent motion — useful when a camera follows a
     vehicle node, for example).
3. **Apply scene-wide visibility filters** (debug toggles, pass enables).

Frustum / occlusion culling are **not** part of v1 extraction. They slot in
here later as a filter step before per-component emission.

### 5.3 Scene-graph traversal vs. asset traversal  **[F]** **(S)**

The extractor walks the scene tree once. Inside, it talks to the asset
registry by ID for any data it needs to resolve (mesh handles, voxel
volumes). The registry is **never traversed** — only queried. This keeps
extraction O(active scene nodes), independent of how many assets are loaded
in the registry.

---

## 6. Camera, lighting, and skybox become Scene members

### 6.1 Camera moves into the Scene  **[E]** **(S)**

Today: `RenderingSystemConfig::camera` is a `shared_ptr<Camera>` injected
from the application; the camera object is a free-standing instance.

After: the active camera lives on the `Scene`. The application still
constructs a `Camera` (so the input system can drive it as before), but it
hands ownership to the scene. Cameras *can* be attached to nodes via
`CameraComponent` to ride along with a parent transform — but the simple
"free camera with mouse-look controls" stays supported by attaching it to a
root node with identity transform and letting `CameraController` write
directly to the camera's TRS.

This is a small change at v1 (the inspector / controller wiring stays the
same), but it puts the camera in the right architectural slot for the
future: cinematic cameras, multiple cameras (split-screen, portals), saved
viewpoints.

### 6.2 SceneLighting moves out of Renderer, into Scene  **[E]** **(S)**

Today: `SceneLighting` lives on `Renderer` ([Renderer.h:111](src/rendering/Renderer.h#L111)).
That's a layering smell — lighting is *scene state*, not renderer state.
Techniques and post-process effects already access it via `RenderContext`,
which means moving it is a small mechanical change: `RenderContext::lighting`
sources from `m_scene.GetLighting()` instead of `m_renderer.GetLighting()`.

A `LightComponent` (§2.2) on a scene node lets us drive sun direction from
node transforms when the user wants that. When no `LightComponent` exists,
the scene's default `SceneLighting` (the spherical sun-az/el controls we
have today) is used unchanged.

### 6.3 Skybox as a scene reference, not a technique concern  **[F]** **(M)**

Sky rendering is currently fused into the brickmap and animated-geometry
techniques' shader code (sun-disk + ambient gradient inside the trace
shader). For a real scene graph, sky is a top-level scene property:

- `Scene::sky` references either a procedural sky description (current
  sun-disk + gradient) or, eventually, a sky cubemap / equirect asset from
  the registry.
- The sky description is *exposed to techniques* via the same
  `RenderContext::scene` pointer; technique shaders read it as data, not as
  hardcoded gradient math.
- A future "Skybox" technique (or pass) renders the sky into the scene
  color attachment at clear-time. Until then, techniques continue to bake
  the sky inline but read parameters from the scene description rather than
  a per-technique copy.

This is the smallest of the §6 items in code, but the largest in *intent* —
sky as scene data is what lets us swap "sunset on a planet" for "underground
cavern" without touching technique code.

---

## 7. RenderingSystem becomes the seam between Scene and Renderer

### 7.1 RenderingSystem owns the Scene + AssetRegistry + Extractor  **[F][I]** **(S)**

Three new members:

- `Scene m_world`
- `AssetRegistry m_assets`
- `SceneExtractor m_extractor`

The application constructs the scene during `Init()` (loads default mesh,
default voxel volume, places a camera node, places a default light). After
that, the application holds the scene by reference for editor wiring; the
RenderingSystem drives extraction.

`RenderContext` extends to carry `Scene*` + `AssetRegistry*` for techniques
that need them at register-pass time (e.g. for binding global resources like
the palette). At record time, only `RenderScene*` is needed — that already
flows through `PassContext`.

### 7.2 The "rebuild graph because Y changed" set shrinks  **[T]** **(S)**

Today, several events trigger graph rebuilds (model reload, .vox reload,
volume size change). After the scene module:

- Asset reload → registry repopulates the persistent buffer in place. **No**
  graph rebuild needed unless the buffer *size* changed (which it does for
  OBJ swaps).
- Adding/removing a scene node → no rebuild. Extraction picks up the change
  next frame.
- Changing the active camera → no rebuild. Extractor reads the new camera.

Graph rebuilds remain only when the *graph topology* changes (technique
switch, persistent-buffer size change, viewport resize). This is a real
ergonomics win — most scene authoring becomes free.

---

## 8. Editor integration

### 8.1 Hierarchy panel  **[E]** **(M)**

A new `HierarchyPanel` shows the scene tree. Selecting a node routes that
node into the existing `InspectorPanel`, which already accepts `IInspectable*`.
SceneNode implements `IInspectable` to expose its TRS and per-component
parameters. This means the inspector becomes the single editor for:

- Render technique parameters (existing)
- Post-process effect parameters (existing)
- SceneLighting (existing)
- **Scene nodes** (new)
- **Asset properties** (new — pick a node's `MeshComponent`, see its
  `AssetID`; inspector lets you reload, swap, etc.)

### 8.2 Asset browser  **[E]** **(M, deferred)**

A panel that lists registry contents grouped by type. v1 can be a flat list;
v2 adds previews, drag-into-hierarchy, etc. Defer until at least one workflow
(foliage editor) wants to browse assets.

### 8.3 Inspector for components  **[E]** **(S)**

Components implement a tiny `GetParameters()` so the inspector reuses the
existing `TechniqueParameter` row-rendering. `MeshComponent` exposes the
asset path (with a File parameter, like the current MeshRasterizer);
`InstanceCloudComponent` exposes count, density, AABB, asset reference.

---

## 9. Forward-looking — what this unlocks

These are the features the user enumerated. Listed here so the priorities in
§10 can be judged against them.

| Feature | Depends on | Unlocked by |
|---|---|---|
| Procedural foliage workflow | §3 (registry holds generated assets), §5 (compute passes write into registry-owned images), §6.3 (sky is scene data) | The registry's `AnimatedVoxelAsset` slot is exactly where compute-generated assets land; the scene references them by ID |
| Animated instanced geometry technique | §2.2 (`InstanceCloudComponent`), §3 (registry owns voxel asset image), §4.3 (bindless asset binding) | The technique becomes ~150 LOC: one pass, one pipeline, iterates `InstancedVoxelMesh` items |
| Multi-technique composition | §6 (lighting + sky are scene-level), §7 (RenderingSystem orchestrates) | A single scene drives all active techniques' producers; today each technique has its own world |
| Frustum / occlusion culling | §5 (extractor is the central filter point) | Insert as a step in `Extract()` between traversal and emission |
| Save / load scenes | §2 (scene tree is serializable POD), §3 (assets serialize as IDs + descs) | Tree → JSON / binary; assets re-load on world load |
| Multiple cameras / portals / split-screen | §6.1 (camera lives in scene) | A second pass with a different camera reads from the same scene |

Two items in §10 (the §3 registry and the §5 extractor) are the
load-bearing ones. If schedule is tight, these are the ones to land first.

---

## 10. Suggested execution order

In dependency order. Each step leaves the engine working — no flag-day
rewrites.

1. **§6.2 Move SceneLighting from Renderer to (a temporary) RenderingSystem
   member.** (S, no risk; pure relocation. Sets up the namespace for §6 once
   Scene exists.)
2. **§3.2–§3.3 AssetRegistry skeleton with `MeshAsset` and `VoxelVolumeAsset`.**
   (M. Make `MeshRasterizer` and `BrickmapPaletteRenderer` allocate through it
   instead of holding their own buffers. Each technique still triggers the
   load, but the data lives in the registry.)
3. **§2.1–§2.2 Scene + SceneNode + initial component variants.** (M. Empty
   tree, no extraction yet. Just the data structure.)
4. **§5 SceneExtractor v1.** (M. Walks the tree, emits Mesh and BrickmapVolume
   items. Replace `EmitItems()` in the two existing techniques with
   "construct a scene node carrying this mesh once at startup" plus extraction.)
5. **§4.1 Remove `EmitItems()` from the technique virtual surface.** (S. Fall
   out of step 4. Techniques are now pure consumers.)
6. **§6.1 Camera moves onto Scene.** (S. Mechanical.)
7. **§7.1 RenderingSystem owns Scene + Registry + Extractor cleanly.** (S.
   Cleanup step after the above.)
8. **§8.1, §8.3 Hierarchy panel + node inspector.** (M. First user-facing
   payoff.)
9. **§6.3 Sky as scene data.** (M. Refactor the trace shaders to read sky
   parameters from a scene-bound UBO instead of hardcoded uniforms.)
10. **§2.2 (`InstanceCloudComponent`) + §4.3 bindless per-item assets.** (L.
    The launchpad for the instanced-animated-geometry technique. Last step
    of *this* doc; first step of the foliage / instance feature work.)

After step 5, the techniques look almost the same as today — slightly slimmer
because asset loading is gone — but the producer/consumer line is clean and
every subsequent feature lands as a scene-side change, not a technique-side
one. After step 10, the foliage technique can be written without touching
anything in `src/rendering/` or any other technique.

---

## 11. Things considered and explicitly *do not* recommend

- **A full ECS (entt-style).** Tempting for the millions-of-instances case,
  but `InstanceCloudComponent` already collapses that case to a single
  bulk buffer. An ECS would over-architect the rest of the scene (a few
  hundred top-level nodes don't benefit from archetype tables) and impose
  a learning curve. Skip until / unless we hit performance bottlenecks
  that the tagged-union approach cannot solve.
- **Component class hierarchy with virtuals.** Cache-unfriendly, painful to
  serialize, and we already gain nothing over the variant approach for the
  small set of component types we actually need.
- **Generic "node visits self" virtual on SceneNode.** Same anti-pattern in
  a different costume. Extraction should be a switch on component type in
  one place, not scattered overrides.
- **Scene scripting / behavior.** Not a game engine; not in scope.
- **A separate "scene compiler" step.** Some engines bake scenes offline.
  We don't have the asset-pipeline infrastructure for that and don't need
  it — extraction at frame time is fine for our scale.
- **Inheriting RenderItem from a Component class.** Keeps the producer and
  the consumer types confusingly braided. RenderItem is the wire format;
  Component is the source. Different lifetimes, different shapes; keep
  them separate.

---

## 12. Open questions for the user

These would change the priority ordering — flag them before starting:

1. **Is there a planned editor mode that operates on a *different* scene
   from the one being rendered?** (e.g. the foliage editor opens a "voxel
   model authoring" scene with a turntable camera, separate from the main
   world). If yes, `RenderingSystem` should hold a *current* scene pointer
   and support swap; if no, we can hardcode one.
2. **Are scenes ever loaded from disk in v1, or always constructed in code?**
   Affects whether §10 step 3 needs to design with serialization in mind from
   day one. v1 in-memory-only is much faster to land.
3. **Should `SceneLighting` (the global "sun") and `LightComponent`
   (per-node lights) coexist, or do we want a single uniform light model?**
   Coexistence is simplest for v1 (the existing brickmap shader keeps
   working unchanged); a unified model is more general but a bigger refactor.
4. **What's the memory budget for an active scene?** Affects whether the
   extractor must be incremental (only re-emit dirty subtrees) from day one,
   or whether full re-emission per frame is fine. For "thousands of
   top-level nodes," full re-emission is fine; for "hundreds of thousands,"
   it isn't.
5. **Will the foliage editor produce assets that get *committed* into the
   scene, or assets that live only inside the editor session?** If the
   former, the registry needs an "import from editor session" path; if the
   latter, the editor has its own throwaway registry and we don't need to
   wire it into the main one.
