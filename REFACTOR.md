# REFACTOR.md — Rendering Architecture Plan

A reading of the current rendering stack and a prioritized plan for tightening
the abstraction before adding scene graphs / streaming / instanced animated
geometry on top. This document is **descriptive and prescriptive only — no code
is written**. Each section names what to change, why, and roughly how
disruptive it is.

The rough mental model of the stack today is:

```
Application
   └─ Renderer (owns RenderGraph + PostProcessChain + SceneLighting)
         ├─ RenderGraph (resources, passes, barriers, framebuffers)
         │     ├─ GraphicsPassBuilder
         │     └─ ComputePassBuilder
         ├─ PostProcessChain (chain of PostProcessEffect)
         └─ RenderTechnique (one of: mesh, brickmap, animated)
                  └─ uses VWrap directly for buffers/images/pipelines/descriptors
```

The diagnosis below splits findings by layer.

---

## Top-line summary

The render graph **knows** what each pass reads and writes (it must, in order to
synthesize barriers and framebuffers), but the per-technique code mostly **re-states
the same information by hand** when wiring descriptors, recording commands, and
recreating pipelines. The biggest leverage refactor is to make the graph the
authority on resource → descriptor binding so techniques only express what is
unique about them: pipeline state, push constants, and the actual draw/dispatch
logic.

Concretely, after the reorg:

- A new technique should be ~150 LOC, not ~500.
- Adding a new compute or graphics stage should not require touching descriptor
  pools, pool sizes, write structs, layout transitions, or pipeline render-pass
  rebinding.
- The `RenderGraph` should support out-of-order pass declaration and basic DAG
  reordering / pruning, even if you don't immediately use it.
- VWrap should be a clean RAII layer; no application code should reach into raw
  `Vk*` structs except inside record callbacks for niche features the wrappers
  don't yet cover.

Below, items are tagged:

- **[L]** load-bearing for future engine-level features (scene graph, streaming, instances)
- **[Q]** quality-of-life / boilerplate reduction
- **[T]** tech-debt cleanup
- Effort: **S** = under a day, **M** = a few days, **L** = week+

---

## 1. Render Graph — make it the authority on resource lifetime and binding

### 1.1 Promote ImageHandle / BufferHandle to safe handles  **[T]** **(S)**

Current: `ImageHandle { uint32_t id }` is a thin alias for an index into
`m_images`. Every `Get*()` does `assert(handle.id < m_images.size())`, but
nothing protects against handles that out-live a graph rebuild (rebuild
clears the vectors, so old handles silently alias new resources).

Plan:

- Add a generation/version field — `{ uint32_t id; uint16_t gen; uint16_t kind; }`
  where `kind` distinguishes graph-level handles (e.g. transient image vs
  imported vs persistent — see §1.4).
- On `Clear()`, bump generation. Validate gen on any `Get*()`.
- This is purely additive; existing call sites compile unchanged because the
  struct constructor stays the same.

### 1.2 Decouple pass declaration order from execution order  **[L]** **(M)**

Current: `m_executionOrder` is just `m_graphicsPasses` and `m_computePasses`
appended in source order. Barriers are computed against this fixed order.
There is no DAG, no topo sort, no culling of unreachable passes.

Why it matters: scene graphs and resource streaming push you toward "register
all passes the engine knows about, then ask the graph to figure out which ones
actually feed the final output, and in what order." Today, if a technique
registers 10 passes and only 4 produce images on the path to the swapchain,
all 10 still execute.

Plan:

- Build an explicit DAG from declared `Read`/`Write` sets after `Compile()`.
  The `m_executionOrder` becomes the topological sort *result*, not the input.
- Add a "sink" concept: passes downstream of the imported swapchain image are
  retained; everything else is pruned (or kept under a debug flag).
- This unlocks: pass enabling/disabling without code paths in techniques (the
  graph drops disabled passes and re-resolves), conditional passes (e.g. skip
  bloom when intensity is 0), and async-compute candidate detection later.
- Keep the linear-mode path as a fallback so the migration is non-breaking.

### 1.3 Tighten barrier synthesis  **[T]** **(M)**

Current shortcomings in `RenderGraph::ComputeBarriers`:

- Compute reads always force `VK_IMAGE_LAYOUT_GENERAL`. Graphics reads always
  force `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. There is no way for a
  compute pass to declare it wants to **sample** an image (which it might if
  it's writing into a different volume — sampling an SDF for instance).
- Buffer reads emit a barrier even when `lastBufferWriter` is the initial
  `TOP_OF_PIPE / 0` sentinel — a no-op barrier wasted in the command stream.
  Same for the per-frame "first read of this resource" case.
- Read-after-read transitions still emit a barrier whenever the layout
  matches but `lastImageWriter` hasn't changed.
- Stage flags for graphics buffer reads always OR vertex+fragment, even when
  only vertex is using the SSBO (mesh rasterizer's UBO).

Plan:

- Extend `Read()`/`Write()` on the builders to accept an explicit
  `ResourceUsage` enum: `SampledRead`, `StorageRead`, `StorageWrite`,
  `VertexBuffer`, `IndexBuffer`, `UniformRead`, `IndirectArg`. The graph maps
  these to layouts, access masks, and stage flags.
- Default callers (no usage specified) keep current behavior, so the migration
  is opt-in.
- Drop sentinel-source barriers; track "has been written this frame" instead
  of relying on default `TOP_OF_PIPE` hops.
- This is a prerequisite for subpass merging or split barriers later.

### 1.4 Add explicit resource lifetime classification  **[L]** **(M)**

Current: every non-imported resource is treated as transient and torn down on
`Resize`, with one hardcoded exception (`VK_IMAGE_TYPE_3D` images are
preserved at line 789 of `RenderGraph.cpp` — done specifically because the
brickmap volume holds uploaded `.vox` data).

Plan:

- Resources have a lifetime: `Transient` (rebuilt every Compile), `Persistent`
  (survives Compile/Resize), `Imported` (external, view may rotate). The
  hardcoded 3D-volume exception goes away.
- `CreateImage` / `CreateBuffer` take a `lifetime` parameter, defaulting to
  `Transient` to preserve current behavior. Brickmap and animated-geometry
  switch to `Persistent`.
- This also lets the graph eventually do **resource aliasing**: two transient
  images with non-overlapping last-use/first-use can share the same VkImage
  allocation. Not urgent, but free perf once the DAG is in place.

### 1.5 Move framebuffer/render-pass invalidation into the graph  **[Q]** **(S)**

Current: there's a known footgun documented in `Application::BuildRenderGraph`
(line 125):

> Techniques create their graphics pipeline inside RegisterPasses using the
> render pass returned by GetRenderPassPtr(), but Compile() later resets each
> graphics pass's VkRenderPass and creates a fresh one. Recreate pipelines
> now so they bind against the post-Compile render pass.

So techniques *must* be ready to be told "your pipeline is invalid, recreate
it" after every Compile. This responsibility leaks into Application.

Plan:

- Don't expose a render pass to the technique inside `RegisterPasses` at all.
  The technique declares vertex format / push constants / shader stages /
  rasterizer state via a `PipelineDesc`, and the graph instantiates the
  `VkPipeline` against the correct render pass after Compile.
- `RecreatePipeline` becomes a graph operation: recreate every pipeline owned
  by any pass. Hot-reload is one line in the graph instead of a fan-out across
  Application + Renderer + every effect.

---

## 2. RenderTechnique abstraction — split lifecycle, parameters, and rendering

### 2.1 Reduce the virtual surface on `RenderTechnique`  **[T]** **(S)**

Current interface (12 virtuals):

```
GetName, RegisterPasses, Shutdown, OnResize, GetShaderPaths,
RecreatePipeline, WriteGraphDescriptors, GetParameters, GetFrameStats,
SetWireframe / GetWireframe, NeedsReload / PerformReload, NeedsRebuild
```

This is mixing rendering, presentation (parameters for ImGui), profiling
(FrameStats), and lifecycle (NeedsRebuild polling).

Plan:

- Keep on the rendering interface only what is actually about rendering:
  `RegisterPasses`, `OnResize`, `Shutdown`. Push pipeline recreation into the
  graph (§1.5) — the technique no longer owns its pipeline directly.
- Move parameter exposure into a sibling interface (`IInspectable` or
  similar) so non-technique objects (post-process effects, lighting, future
  scene graph nodes) implement the same contract. The Editor inspector can
  then walk one polymorphic surface instead of three (technique parameters,
  post-process parameters, lighting struct).
- Replace polled `NeedsReload`/`NeedsRebuild` flags with an event channel
  (the `AppEvent` queue already exists — extend it rather than poll).

### 2.2 Introduce a "PassResources" handle the technique works against  **[L]** **(M)**

Current: each technique re-creates from scratch the descriptor layout +
descriptor pool + N descriptor sets + sampler creation + manual
`vkUpdateDescriptorSets` after Compile and again after every resize. There is
a `DescriptorSetBuilder` for layouts, but the *write* side is fully manual —
this is the single largest source of duplicated code (compare
`BrickmapPaletteRenderer::WriteGraphDescriptors` against
`AnimatedGeometryRenderer::WriteGraphDescriptors` against
`BloomEffect::WriteGraphDescriptors`).

Plan:

- Extend `DescriptorSetBuilder` (or add a sibling `BindingTable` builder) so
  binding declarations carry not just `(binding, type, stage)` but also
  *what they bind to* — an `ImageHandle` + sampler, or a `BufferHandle`, or a
  per-frame mapped UBO.
- The graph holds onto these binding tables. After `Compile`, it auto-writes
  every binding (and re-writes them on resize when the underlying view
  changes). Techniques no longer implement `WriteGraphDescriptors` for
  bindings that point at graph-managed resources — they just declare them and
  the graph does the rest.
- Per-frame UBO bindings (the mesh rasterizer's UniformBufferObject) are a
  special case — the binding table can hold N descriptors, one per frame in
  flight, and the graph wires them.
- **Estimated impact:** removes 30–80% of the LOC in every technique and
  every post-process effect. Eliminates the entire `WriteGraphDescriptors`
  overload dance.

### 2.3 Standard "fullscreen quad effect" base class  **[Q]** **(S)**

Currently, the bloom passes and lens flare follow the exact same shape:
- Single graphics pass writing one color attachment, reading 1–2 sampled inputs.
- 4-vertex `vkCmdDraw(cmd, 4, 1, 0, 0)`.
- `PipelineDefaults::FullscreenQuad`.
- Single push constant block.
- Manual binding of the pipeline + descriptor set + push.

This is by far the most common shape and warrants a helper:

- A `FullscreenPass` builder that takes `{name, output, reads, fragSpv,
  pushConstants, recordPushFn}` and produces the entire pass + pipeline.
- All four bloom stages and the lens flare collapse to ~10 lines each.
- The boilerplate `addPass` lambda inside `BloomEffect::RegisterPasses` is a
  hint that this abstraction wants to exist.

### 2.4 Introduce a "RenderItem" / drawable abstraction  **[L]** **(L)**

This is forward-looking work, not cleanup. The mesh rasterizer hardcodes a
single OBJ as a single draw. The future you describes (instances, animated
geometry, scene graphs) wants a layer between "the technique" and "geometry
to draw":

- A `RenderItem` knows: vertex/index buffer ranges, instance count, material
  bindings, transform, per-instance data offset.
- Techniques don't own geometry — they consume `RenderItem` lists. The scene
  graph emits items; techniques interpret them.
- This is the cleanest place to draw a line between **renderer** (frame
  graph + technique + lighting) and **scene** (entities + transforms +
  drawables). Get this line in early, even if the first version is just
  "MeshRasterizer takes a list of one item."

Defer until §1, §2.1, §2.2 land — those are foundational; this is the first
*new* feature on top.

---

## 3. VWrap — close the leakage

### 3.1 Delete `OffscreenTarget`  **[T]** **(S)**

It has no consumers outside its own .cpp/.h. The render graph supersedes it.
Easy win.

### 3.2 Add the missing CommandBuffer wrappers  **[Q]** **(S)**

Inconsistency: `CommandBuffer` has `CmdBindComputePipeline`,
`CmdBindComputeDescriptorSets`, `CmdDispatch`, `CmdPipelineBarrier`. But
graphics pipelines are bound with raw `vkCmdBindPipeline` everywhere
(checked: 5 different files). There's no `CmdPushConstants`,
`CmdDrawIndexed`, `CmdBindVertexBuffers`, `CmdSetViewport`, `CmdSetScissor`
wrapper.

Plan:

- Add the obvious ones. Each is 3–5 lines. Most should take VWrap pointers,
  not raw handles, where it makes sense.
- Together with §2.3, this should drop the 42 direct `vk*` calls counted in
  `src/` to fewer than 10, all in places where wrappers genuinely don't
  apply.

### 3.3 Expose more of `Sampler`'s VkSamplerCreateInfo  **[T]** **(S)**

`AnimatedGeometryRenderer.cpp:152-166` creates a raw `VkSampler` because
`VWrap::Sampler::Create(device)` only does linear filtering. Either expose
the full create info, or add named factories: `Sampler::CreateLinearClamp`,
`Sampler::CreateNearestClamp`. Same for the palette resource which presumably
does this too.

### 3.4 Stop folding `RenderPass` factories that are now unused  **[T]** **(S)**

`RenderPass::CreateImGUI` and `RenderPass::CreateOffscreen` are vestigial
(after the graph took over). Confirm and delete. `CreatePresentation` is
still used by `Application::InitVulkan` for ImGui setup — keep that one.

### 3.5 Wrap `VkPipelineLayout` and `VkSampler` consistently  **[T]** **(S)**

Pipeline layout creation is buried inside `Pipeline.cpp` and
`ComputePipeline.cpp`. There's no reuse. Hoist it into a tiny
`PipelineLayout` class so multiple pipelines built against the same
descriptor-set-layout / push-constant-range share one layout (also Vulkan-
correct: pipelines from the same layout can share descriptor binds).

---

## 4. Renderer — separate "engine concerns" from "scene concerns"

### 4.1 Don't hardcode the scene image stack inside `Renderer`  **[L]** **(M)**

Current: `Renderer::Build` (lines 31-46) creates `scene_color`, `scene_depth`,
`scene_resolve` and hands them to the active technique. This baked-in
assumption — "every technique wants MSAA color + depth + resolve, in
swapchainFormat, at offscreen extent" — won't survive future techniques
(deferred-shading G-buffers, voxel cone tracing radiance volumes, anything
that doesn't use depth at all).

Plan:

- The technique declares its target needs:
  `RenderTargetDesc { formats[], samples, hasDepth, ... }`
- The Renderer fulfills the request — creating exactly the images the
  technique asked for, then connecting the technique's "final scene image"
  output handle into the post-process chain.
- This change is small in code but conceptually large: the Renderer becomes
  a coordinator, not a structure-imposer.

### 4.2 Move the post-process chain registration into `Renderer::Build`'s body without special-casing  **[Q]** **(S)**

Today `PostProcessChain::Register` returns the final image, which the
Renderer plugs into the UI pass. This is fine for one pre-built chain but
will get awkward if/when techniques *also* want to opt into specific chains
(e.g., "only run bloom in the brickmap technique"). The right answer is
probably to express the chain as a series of declarative steps the graph
appends, not a procedurally-built pass list.

Defer until you actually have a second chain. Today this is OK as-is.

### 4.3 Hoist all the "rebuild on X" wiring out of `Application`  **[Q]** **(M)**

`Application` currently owns:
- The technique list and active index
- Event queue with HotReloadShaders / SwitchRenderer / DpiChanged / Screenshot
- Every "must rebuild graph because Y changed" call site
- Polling for `NeedsReload` / `NeedsRebuild` after every event flush

This is *coordination logic*, not *application bootstrapping*. Pull it into
a `RenderingSystem` (or `Engine`) class that sits between Application and
Renderer, owns the technique selection, and exposes a small surface to
Application: `Update(dt)`, `RenderFrame(cmd)`, `RequestReload()`,
`RequestSwitchTechnique(idx)`.

After this, `Application` is mostly: window lifecycle, input bootstrap,
imgui bootstrap, main loop. ~150 lines.

---

## 5. Forward-looking — what unlocks the modules you mentioned

These are not refactors — they are the features you want to add. Listed here
so the refactor priorities above can be judged against them.

| Feature | Depends on | Why |
|---|---|---|
| Scene graph | §2.1, §2.4 | Scene = source of `RenderItem`s; technique = consumer |
| Procedural geometry | §2.4, §1.4 (persistent buffers) | Need to upload-then-keep generated meshes |
| Resource streaming | §1.2 (DAG), §1.4 (lifetime) | Eviction needs to know last-use frames per resource |
| Async compute | §1.2, §1.3 | Need DAG + accurate stage flags before splitting queues |
| Indirect / GPU-driven culling | §1.3 (IndirectArg usage), §2.4 | Cull on GPU emits draw args; the technique consumes them |
| Subpass merging (mobile) | §1.3 (read-as-input-attachment), §1.5 | Requires knowing which sampled reads can fold into the producer pass |

Notice that two refactor items (§1.2 DAG, §1.3 barrier classes) unlock most
forward features. If you do nothing else, do those.

---

## 6. Suggested execution order

I recommend doing these in dependency order. Each item leaves the engine in
a working state (no flag-day rewrites).

1. **§3.1, §3.4** — delete dead VWrap classes. (S, no risk, clarifies layering.)
2. **§3.2, §3.3, §3.5** — flesh out VWrap wrappers. (S, lots of small
   commits.)
3. **§2.3** — `FullscreenPass` helper. (S, immediately collapses ~150 lines
   in bloom/flare.)
4. **§1.5** — graph owns pipeline against post-Compile render pass. (S,
   removes the largest documented footgun.)
5. **§2.2** — auto-write descriptors from binding tables. (M, biggest LOC
   payoff. Migrate one technique at a time.)
6. **§2.1** — split `RenderTechnique`'s virtual surface. (S, but requires
   §2.2 to land first since several virtuals collapse afterwards.)
7. **§1.1, §1.4** — handles + resource lifetimes. (S each, additive.)
8. **§1.3** — typed `ResourceUsage` on Read/Write. (M, opt-in migration.)
9. **§4.1, §4.3** — Renderer is a coordinator; RenderingSystem above it.
   (M, mechanical once §2.1 is done.)
10. **§1.2** — DAG + topological order + pruning. (M-L, the biggest single
    item; do it last when the graph's contract is otherwise stable.)
11. **§2.4** — RenderItem / drawable abstraction. (L, this is the launch pad
    for the scene graph and instance work.)

After step 5, `MeshRasterizer.cpp` should be ~250 lines (was 513),
`AnimatedGeometryRenderer.cpp` ~150 lines (was 292), and `BloomEffect.cpp`
~120 lines (was 269).

---

## 7. Things I considered and explicitly *do not* recommend

- **Templated/CRTP techniques.** Adds compile-time complexity, gains nothing
  over a clean polymorphic interface for this codebase's scale. Skip.
- **A custom render-graph DSL or YAML.** Massive overkill. The fluent C++
  builders are fine; just polish them.
- **Dynamic rendering (VK_KHR_dynamic_rendering)** as a general replacement
  for VkRenderPass. Real benefit, but it's a separate refactor with its own
  risks (driver maturity, MoltenVK interaction); pursue it after the
  graph-DAG work, if at all.
- **Vulkan 1.3 sync2 (`VkPipelineStageFlags2`)**. Worth doing eventually for
  cleaner barrier expression, but only after §1.3 has classified all
  resource usages — otherwise you're just porting the existing imprecision
  to a fancier API.
- **Replacing VWrap with vk-bootstrap / vk-hpp.** VWrap is good enough; the
  cost of swapping is high and the gain is mostly stylistic.

---

## 8. Open questions for you

These would change the priority ordering — flag them before I'd start:

1. **Will the engine ever need to run on tile-based / mobile GPUs?** If yes,
   subpass merging matters and §1.3 + §1.5 should explicitly track
   input-attachment usage. If no, skip it.
2. **Is async compute on the roadmap?** If yes, §1.2 (DAG) becomes
   higher-priority. If no, the current linear order is genuinely fine.
3. **How dynamic is the scene-graph plan?** A static "load OBJ, render once"
   scene graph is a different beast from a streaming open-world one. The
   §1.4 lifetime work scales with the answer.
4. **Hot-reload UX intent.** Today, hot-reload is a manual button. If you
   want file-watch-driven hot-reload, the §4.3 `RenderingSystem` extraction
   is where the file watcher would live and its design would change
   slightly.
