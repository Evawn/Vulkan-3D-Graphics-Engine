# AUDIT.md — Architecture & Code Quality Audit

A comprehensive technical audit of the Vulkan voxel rendering engine. This document is split into two parts:

1. **Architecture Overview** — how the application is organized and how it actually works, layer by layer, so you can navigate and reason about the code.
2. **Evaluation** — quantitative ratings and qualitative analysis of code quality, architectural design, maintainability, and extensibility.

The audit was produced by reading the codebase end-to-end (~11k LOC of application code in [src/](src/), ~4k LOC of [VWrap](lib/VWrap/), the two design documents [REFACTOR.md](REFACTOR.md) and [SCENE-GRAPH.md](SCENE-GRAPH.md), and shaders).

---

## Part 1 — Architecture Overview

### 1.1 What this engine actually is

A C++20 / Vulkan rendering engine that exists primarily as a *platform* for experimenting with rendering techniques — three are currently shipped:

- **MeshRasterizer** — classical indexed triangle rasterization with OBJ/MTL loading.
- **BrickmapPaletteRenderer** — DDA voxel ray-casting through a two-level brickmap acceleration structure with a 256-entry palette.
- **AnimatedGeometryRenderer** — single-level DDA against an animated procedural voxel volume; scaffold for the future foliage / instanced-voxel work.

Wrapped around them is an ImGui docking editor (viewport, metrics, output log, inspector, render-graph panel), GPU profiling via timestamp queries, shader hot-reload, and screenshot capture. Most of the engine is content-agnostic — voxels are one consumer among several.

### 1.2 Layered architecture (top to bottom)

```
┌────────────────────────────────────────────────────────────────────┐
│ main()  →  Application (window/loop bootstrap)                     │
├────────────────────────────────────────────────────────────────────┤
│ Editor                            │  RenderingSystem                │
│   ImGui + panels                  │   coordinator: techniques,      │
│   (Viewport / Inspector /         │   event queue, profiler,        │
│   Metrics / Output / RenderGraph) │   build orchestration           │
│                                   │     │                           │
│                                   │     ▼                           │
│                                   │  Renderer                       │
│                                   │   owns RenderGraph,             │
│                                   │   PostProcessChain, lighting    │
│                                   │     │                           │
│                                   │     ▼                           │
│                                   │  RenderGraph                    │
│                                   │   resources, passes, DAG,       │
│                                   │   barriers, pipelines, async    │
│                                   │     ▲                           │
│                                   │     │ declares                  │
│                                   │  RenderTechnique  +             │
│                                   │  PostProcessEffect (consumers)  │
├────────────────────────────────────────────────────────────────────┤
│ VWrap (RAII wrapper over raw Vulkan: Instance/Device/Pipeline/...) │
├────────────────────────────────────────────────────────────────────┤
│ Vulkan + VMA + GLFW + GLM + spdlog + ImGui                         │
└────────────────────────────────────────────────────────────────────┘
```

The dependency direction is strictly downward: `Application` knows about everything; `RenderingSystem` knows about `Renderer` + techniques; `Renderer` knows about `RenderGraph`; `RenderGraph` knows only `VWrap`; `VWrap` knows only Vulkan.

### 1.3 Layer-by-layer walkthrough

#### `main` and `Application` ([main.cpp](src/main.cpp), [Application.h](src/Application.h), [Application.cpp](src/Application.cpp))

[main.cpp](src/main.cpp) is 27 lines: it initializes the logger, creates an `Application`, runs it, and reports fatal exceptions. Heavy single-translation-unit `#define`s for STB / TinyObjLoader / VMA live here so the implementations are compiled exactly once.

[Application](src/Application.h) is a deliberately thin orchestration layer (~190 LOC). Its responsibilities:

- Window lifecycle ([Window](src/Window.h), GLFW).
- Vulkan bootstrap via `VWrap::VulkanContext::Create` ([VulkanContext.h](lib/VWrap/include/VulkanContext.h)).
- Editor and panel construction.
- Camera ([Camera.h](src/Camera.h)) + [CameraController](src/CameraController.h) wiring.
- Constructing and configuring the `RenderingSystem`.
- The main loop: poll input, drain rendering events, draw a frame.

It does *not* own technique selection logic, graph rebuild orchestration, or screenshot capture — those have been hoisted into `RenderingSystem` (REFACTOR.md §4.3 has been executed). What's left is mostly cross-system *wiring*: callbacks that make the editor talk to the rendering system and vice versa.

#### `RenderingSystem` ([RenderingSystem.h](src/rendering/RenderingSystem.h), [RenderingSystem.cpp](src/rendering/RenderingSystem.cpp))

This is the *coordinator* layer between application and renderer. It owns:

- The `Renderer`.
- The vector of `RenderTechnique`s and the active-index pointer.
- The [AppEvent](src/AppEvent.h) queue (HotReloadShaders, SwitchRenderer, ReloadTechnique, RebuildGraph, RecreatePipelines, CaptureScreenshot).
- The `GPUProfiler`.
- A frame-local `RenderScene` that techniques fill via `EmitItems` each frame.
- Application-supplied callbacks for before/after graph rebuild and post-screenshot.

Per frame, `DrawFrame` does roughly:

```
m_scene.Clear();
for each technique: technique->EmitItems(m_scene, ctx);
m_renderer.GetGraph().SetScene(&m_scene);
profiler->CmdBegin → renderer->Execute → profiler->CmdEnd
```

The event queue is drained between frames in `ProcessEvents` ([RenderingSystem.cpp:126](src/rendering/RenderingSystem.cpp#L126)) — a single `vkDeviceWaitIdle` per *batch* of events that need it (rather than per event), and the loop re-runs because event handlers can post follow-ups (e.g. `ReloadTechnique` posting `RebuildGraph`).

#### `Renderer` ([Renderer.h](src/rendering/Renderer.h), [Renderer.cpp](src/rendering/Renderer.cpp))

A thin coordinator (~140 LOC) that:

- Owns the `RenderGraph`, `PostProcessChain`, and `SceneLighting`.
- Asks the active technique what scene-image shape it needs via `RenderTargetDesc DescribeTargets(caps)` and allocates exactly that (color, optional depth, optional MSAA resolve) — this is the result of REFACTOR.md §4.1 (the renderer no longer hardcodes a "color + depth + resolve at swapchain format" scene stack).
- Calls `technique->RegisterPasses(graph, ctx, targets)`.
- Threads the technique's scene-output handle through the post-process chain.
- Imports the swapchain image and registers the UI/presentation pass.
- Calls `graph.Compile()`, then runs `technique->OnPostCompile(graph)` for any one-shot work that needs allocated resources (e.g. seeding a 3D image from `.vox` data).

Three rebuild entrypoints are exposed: `Build` (full), `OnSwapchainResize` (delegates to Build), and `OnViewportResize` (resize-only, keeps swapchain). `Execute` simply delegates to the graph and wires async-compute waits.

#### `RenderGraph` — the keystone ([RenderGraph.h](src/rendering/RenderGraph.h), [RenderGraph.cpp](src/rendering/RenderGraph.cpp), [RenderGraphTypes.h](src/rendering/RenderGraphTypes.h))

This is the largest single file (~1560 LOC) and the architectural keystone. It's responsible for the entire lifecycle of GPU-side resources, passes, and synchronization:

**Resources:** generation-tagged handles (`{ uint32_t id; uint32_t gen; }` — REFACTOR.md §1.1, done) prevent silent aliasing across `Clear()` rebuilds. Resources have a `Lifetime` enum (`Transient` / `Persistent` — §1.4, done): persistent resources survive `Resize()`. Imported resources (e.g. swapchain views) are tracked with their external layout.

**Passes:** declared via fluent builders ([GraphicsPassBuilder](src/rendering/GraphicsPassBuilder.h), [ComputePassBuilder](src/rendering/ComputePassBuilder.h)) sharing [PassBuilderBase](src/rendering/PassBuilderBase.h). Each pass declares:
- Read/write image and buffer handles, each tagged with a `ResourceUsage` enum (`SampledRead`, `StorageRead`, `StorageWrite`, `UniformRead`, `VertexBuffer`, `IndexBuffer`, `IndirectArg`, `Default`) — REFACTOR.md §1.3, done. Drives precise layout/access/stage barrier synthesis.
- A `GraphicsPipelineDesc` *factory* (closure), not a `VkPipeline`. The graph instantiates it after `Compile()` against the canonical render pass (REFACTOR.md §1.5, done — the documented footgun is gone).
- A record callback that emits the actual draws/dispatches.
- A list of `RenderItemType`s the pass accepts (`AcceptsItemTypes(...)`), today documentation; future graph-driven dispatch.
- An optional `BindingTable` (see below).

**DAG and pruning** ([PassDAG.h](src/rendering/PassDAG.h), [DAGBuilder.h](src/rendering/DAGBuilder.h), [DAGBuilder.cpp](src/rendering/DAGBuilder.cpp)) — REFACTOR.md §1.2, done. `Compile()` projects each pass's read/write sets into a `DAGBuildInputs` struct, hands it to `DAGBuilder::Build`, which:

- Emits RAW / WAW / WAR edges in declaration order.
- Topo-sorts with a stable lowest-id-wins tie-break (gives declaration-order parity when no semantic reason exists to reorder).
- Identifies sinks: imported resources, `Lifetime::Persistent` resources, and explicit `MarkSink(handle)` calls. Reverse-reachability prunes unreachable passes.
- Resolves `QueueAffinity` per pass with iterative demotion (an `AsyncCompute` pass loses its tag if any dependency is on the graphics stream, propagating forward in topo order).
- Emits `CrossStreamEdge`s for dependencies that span queue families.

The graph then runs `AccumulateUsageFlags` (deriving `VkImageUsageFlags` / `VkBufferUsageFlags` from declared usages), `AllocateTransientResources` (skipping persistent + imported), `CreateRenderPasses` (with intelligent final-layout selection via `IsImageReadDownstream`), `CreateFramebuffers`, `CreatePipelines`, `ComputeBarriers`, and `UpdateBindings`.

**Barrier synthesis** ([RenderGraph.cpp](src/rendering/RenderGraph.cpp#L759)) is the densest single piece of logic in the codebase (~400 lines). Per pass in execution order, it tracks current image layout, last writer's stage+access, owner queue stream, and last producer index. For each declared read/write it either emits an intra-stream barrier or — when the previous owner was a different queue — splits into a *release* (queued onto the producer's command buffer) and *acquire* (queued onto the consumer's). Sentinel-source barriers (`TOP_OF_PIPE → 0` for the first read of a never-written resource) are dropped. Read-after-read with matching layout is dropped.

**Execution** ([RenderGraph.cpp:1442](src/rendering/RenderGraph.cpp#L1442)): if the build produced any async-compute work, the graph records that subset onto a per-frame async command buffer, submits it on the compute queue, signals a binary semaphore, and reports the wait list back through `GetGraphicsQueueWait()` for `Application::DrawFrame` to attach to the host's main submit. The graphics stream is then recorded into the supplied command buffer. `RecordStream` walks `m_executionOrder`, filters by stream, emits pre-pass barriers, the pass body (begin render pass → set viewport → record callback → end), then post-pass release barriers.

**Introspection:** `BuildSnapshot()` returns a `GraphSnapshot` struct (passes, barriers, resources) consumed by [RenderGraphPanel](src/editor/panels/RenderGraphPanel.h) for the dev tooling UI. The snapshot is rebuilt on every graph rebuild.

#### Render techniques ([RenderTechnique.h](src/rendering-techniques/RenderTechnique.h))

A technique is a content-and-passes consumer of the graph. The current virtual surface is small:

- `DescribeTargets(caps) → RenderTargetDesc` — what scene image stack the technique needs.
- `RegisterPasses(graph, ctx, targets)` — declarative pass setup.
- `OnPostCompile(graph)` — one-shot work after graph allocates resources.
- `EmitItems(scene, ctx)` — fill the per-frame `RenderScene`.
- `GetShaderPaths()`, `GetFrameStats()`, `Reload(ctx)`.

Plus the inherited `IInspectable` surface (`GetDisplayName`, `GetParameters`) that the inspector uses to render parameter rows.

The three concrete techniques follow a uniform shape. [MeshRasterizer.cpp](src/rendering-techniques/mesh-rasterizer/MeshRasterizer.cpp) (~500 LOC) is the largest because of OBJ/MTL parsing and fallbacks; the actual graph wiring is ~80 LOC. [BrickmapPaletteRenderer.cpp](src/rendering-techniques/brickmap-palette/BrickmapPaletteRenderer.cpp) (~390 LOC) is the most representative: three passes (compute generate → compute build → graphics trace), three binding tables, all wired declaratively.

#### `RenderItem` and `RenderScene` ([RenderItem.h](src/rendering/RenderItem.h), [RenderScene.h](src/rendering/RenderScene.h))

The scene/technique wire format. `RenderItem` is a POD drawable atom (geometry handles, instance range, voxel asset reference, transform, AABB) tagged by `RenderItemType` (Mesh / Fullscreen / InstancedVoxelMesh / BrickmapVolume). `RenderScene` is a per-type-bucketed flat-vector store, cleared each frame. Free draw helpers (`DrawMeshItem`, `DrawFullscreenItem`, `DrawInstancedVoxelMesh`) live next to the type. This is the seam SCENE-GRAPH.md is built around.

#### `BindingTable` ([BindingTable.h](src/rendering/BindingTable.h), [BindingTable.cpp](src/rendering/BindingTable.cpp))

Declarative descriptor-set wiring (REFACTOR.md §2.2, done). A binding source can be a graph-managed sampled image, storage image, storage buffer, an external image+sampler, or a per-frame UBO array. The table builds the layout, pool, and N descriptor sets in `Build()`; `Update(graph)` re-runs `vkUpdateDescriptorSets` for every binding, re-resolving graph handles. The graph automatically calls `Update` after `Compile()` and after `Resize()` — so techniques never write descriptors by hand.

#### `PostProcessChain` ([post-process/](src/rendering/post-process/), [PostProcessChain.h](src/rendering/post-process/PostProcessChain.h))

An ordered list of `PostProcessEffect`s. `Register(graph, sceneInput, extent)` threads `sceneInput` through each enabled effect and returns the final image handle. Currently: [BloomEffect](src/rendering/post-process/BloomEffect.h) (4 fullscreen passes: bright → blur H → blur V → composite) and [LensFlareEffect](src/rendering/post-process/LensFlareEffect.h). Both built with [FullscreenPass](src/rendering/FullscreenPass.h), a tiny helper (~90 LOC implementation) that collapses a "single output, N sampled inputs, one fragment shader, one push block" pass into ~10 lines (REFACTOR.md §2.3, done).

#### Editor ([editor/](src/editor/), [Editor.h](src/editor/Editor.h))

ImGui docking-mode editor with five panels:
- [ViewportPanel](src/editor/panels/ViewportPanel.h) — displays the offscreen-rendered scene as a sampled texture.
- [MetricsPanel](src/editor/panels/MetricsPanel.h) — FPS, GPU render time, memory stats, perf graphs.
- [InspectorPanel](src/editor/panels/InspectorPanel.h) — walks any `IInspectable*` (technique, post-process effect, lighting); supports parameters of types Float / Int / Bool / Color3 / Color4 / Enum / File.
- [OutputPanel](src/editor/panels/OutputPanel.h) — live spdlog viewer with severity filtering via [ImGuiLogSink.h](src/editor/ImGuiLogSink.h).
- [RenderGraphPanel](src/editor/panels/RenderGraphPanel.h) — visualizes the live `GraphSnapshot` (passes, barriers, per-pass timing, resource references).

Editor exposes callbacks that `Application` wires to `RenderingSystem::Request*` methods. The editor never touches the graph or techniques directly except through the snapshot.

#### `VWrap` — RAII Vulkan wrapper ([lib/VWrap/](lib/VWrap/))

A first-party wrapper library (~4k LOC across 25 wrapper classes). Each Vulkan concept (`Instance`, `PhysicalDevice`, `Device`, `Allocator`, `Surface`, `Swapchain`, `FrameController`, `Queue`, `CommandPool`, `CommandBuffer`, `Buffer`, `Image`, `ImageView`, `Sampler`, `RenderPass`, `Framebuffer`, `Pipeline`, `ComputePipeline`, `PipelineLayout`, `DescriptorSetLayout`, `DescriptorPool`, `DescriptorSet`, `Fence`, `Semaphore`) is a small class with `static Create(...)` factory methods returning `std::shared_ptr`. Lifetime is tied to the shared_ptr; destructors call the `vkDestroy*`. The whole library is bundled in a single `VulkanContext` struct returned by `VulkanContext::Create(window, enableValidation, framesInFlight)`.

The `CommandBuffer` wrapper has selectively gained higher-level helpers (`CmdBindComputePipeline`, `CmdDispatch`, `CmdPipelineBarrier`, `CmdPushConstants`, `CmdCopyBuffer`, …); some graphics calls (vkCmdSetViewport / vkCmdSetScissor / vkCmdBindPipeline graphics) are still raw inside record callbacks — about 16 direct `vk*` calls remain in application code (vs. the >40 the REFACTOR.md baseline cited).

#### Async compute ([RenderGraph.cpp](src/rendering/RenderGraph.cpp), [DAGBuilder.cpp](src/rendering/DAGBuilder.cpp))

When the device exposes a separate compute queue family AND a compute pass tagged `QueueAffinity::AsyncCompute` has no graphics-stream dependency, the graph records that pass into a per-frame async command buffer, submits on the compute queue, and signals a binary semaphore the host attaches to the next graphics submit's wait list. Cross-stream dependencies become queue-family ownership transfers (release on producer, acquire on consumer). Demotion is iterative (cascades correctly), counted, and logged once.

#### Per-frame data flow

```
Window event poll
   │
   ▼
CameraController::Update(dt)        (mouse + keys → camera state)
RenderingSystem::ProcessEvents()    (hot reload, switch, screenshot)
   │
   ▼
Editor::BeginFrame()                (ImGui new frame)
DrawFrame:
   │
   ├─ FrameController::AcquireNext  (acquire swapchain image)
   ├─ rendering.UpdateSwapchainView (rotate import)
   ├─ cmd.Begin
   ├─ rendering.DrawFrame
   │     ├─ scene.Clear, EmitItems on each technique
   │     ├─ profiler.CmdBegin
   │     ├─ renderer.Execute  → graph.Execute
   │     │      ├─ (if async work) record async cb + queueSubmit + semaphore
   │     │      └─ RecordStream(graphics) — barriers, render passes, callbacks
   │     └─ profiler.CmdEnd
   ├─ cmd.End
   └─ FrameController::Render(graphicsWaits, stages)  — present
```

### 1.4 Cross-cutting features

- **Hot-reload (F5)** — collects every shader path from the active technique + post-process chain, runs `ShaderCompiler::CompileAll`, and on success calls `graph.RecreatePipelines()` (re-invokes every pass's pipeline desc factory and rebuilds VkPipelines without rebuilding the graph).
- **Screenshot** — copies the final scene image (post-process output) through a staging buffer to a PNG via stb_image_write ([ScreenshotCapture](src/utils/ScreenshotCapture.h)).
- **Logging** — spdlog with three named loggers ("App", "Render", and the editor's ImGui sink). Initialized in `main`.
- **Profiling** — Vulkan timestamp queries with one query pool per frame-in-flight, two timestamps per pass, fed into the metrics + render-graph panels.
- **Shader build** — `glslc` via CMake's `add_custom_command` in [shaders/CMakeLists.txt](shaders/CMakeLists.txt); the `Shaders` target is a `VulkanEngine` build dependency.
- **Path resolution** — paths embedded in source via a generated `config.h` (`config::SHADER_DIR`, `config::ASSET_DIR`) so the binary works from the build directory without a working-directory contract.

---

## Part 2 — Evaluation

Each of the four axes is rated on a **1–10 scale** and graded with a letter (S / A / B / C / D / F). Ratings reflect the codebase as it stands today, not its trajectory.

| Axis | Score | Grade |
|---|---|---|
| Code quality | 8.5 / 10 | A |
| Architecture & design decisions | 9 / 10 | A |
| Maintainability | 7 / 10 | B+ |
| Extensibility | 8.5 / 10 | A |
| **Overall** | **8.25 / 10** | **A−** |

### 2.1 Code quality — 8.5 / 10 (A)

**Strengths**

- **Idiomatic modern C++.** C++20 standard, RAII via `shared_ptr` factories everywhere in VWrap, no manual `new`/`delete` in app code. `std::variant` for tagged unions ([BindingTable.h](src/rendering/BindingTable.h#L87)), `std::span<const RenderItem>` for non-owning views ([RenderScene.h](src/rendering/RenderScene.h#L35)), `std::function` callbacks for record + factories.
- **Excellent commentary discipline.** Comments consistently explain *why*, not *what*. Examples: the cache stays warm because `m_executionOrder` is a subsequence of `m_declarationOrder` ([DAGBuilder.cpp:192-208](src/rendering/DAGBuilder.cpp#L192-L208)); `TOP_OF_PIPE` substitution in barriers is explicitly justified ([RenderGraph.cpp:1338](src/rendering/RenderGraph.cpp#L1338)); `static_assert(sizeof(BrickmapPaletteTracePC) == 144, …)` documents std140 layout ([BrickmapPaletteRenderer.cpp:36](src/rendering-techniques/brickmap-palette/BrickmapPaletteRenderer.cpp#L36)).
- **Naming is precise and consistent.** `m_` for members, `Pascal` for types and methods, `snake_case` only inside techniques (slight inconsistency — see weaknesses). `Read` / `Write` for graph access, `BindGraphSampledImage` / `BindExternalSampledImage` for binding sources.
- **Defensive but not paranoid.** Handle generation tagging asserts immediately on stale handles ([RenderGraph.cpp:172-179](src/rendering/RenderGraph.cpp#L172-L179)). DAG-cycle detection in debug only ([DAGBuilder.cpp:119-120](src/rendering/DAGBuilder.cpp#L119-L120)). User-facing paths (texture load, model load) fall back to placeholders ([MeshRasterizer.cpp:42-46](src/rendering-techniques/mesh-rasterizer/MeshRasterizer.cpp#L42-L46)) instead of throwing.
- **Few raw Vulkan calls in app code.** REFACTOR.md baseline cited >40 raw `vk*` calls outside VWrap; today ~16 remain, mostly in places where wrappers genuinely don't apply (vkCmdSetViewport/Scissor, ImGui init).
- **Sound concurrency.** Async-compute path uses per-frame command buffers + binary semaphores correctly; the in-flight fence (held by `FrameController`) guarantees retirement before re-record.
- **No memory leaks by construction.** VMA owns GPU memory via `Allocator`; CPU side everything is `shared_ptr` or value-typed.

**Weaknesses**

- **No automated tests.** Zero unit tests, integration tests, or test infrastructure in the repo. For an engine of this complexity (especially the DAG + barrier-synthesis logic, which is fertile ground for subtle correctness bugs), this is the single largest quality gap. The DAG builder asserts a "subsequence invariant" in debug builds ([DAGBuilder.cpp:192](src/rendering/DAGBuilder.cpp#L192)) — a dedicated test suite would catch regressions much faster.
- **Naming inconsistency between layers.** Engine code is `Pascal::Pascal`; technique code mixes `snake_case` member fields (`m_volume_size`, `m_max_iterations`, `m_compute_bindings`). Both styles are internally consistent, but the boundary is jarring.
- **Some files near the comfort ceiling.** [RenderGraph.cpp](src/rendering/RenderGraph.cpp) is 1556 lines in one translation unit. It's logically partitioned by `// =====` banner comments, but at this size further splitting (e.g. `RenderGraphCompile.cpp`, `RenderGraphExecute.cpp`, `RenderGraphBarriers.cpp`) would improve navigability.
- **Magic constants without symbolic names** in places. `28` (byte offset of brick_count atomic) appears as a literal in both `vkCmdFillBuffer` and the comment ([BrickmapPaletteRenderer.cpp:163](src/rendering-techniques/brickmap-palette/BrickmapPaletteRenderer.cpp#L163)); a `constexpr size_t kBrickCountByteOffset = 7 * sizeof(uint32_t);` would document and centralize this.
- **One-line manual `vkCmdPipelineBarrier` inside a record callback** ([BrickmapPaletteRenderer.cpp:165-172](src/rendering-techniques/brickmap-palette/BrickmapPaletteRenderer.cpp#L165-L172)) bypasses graph-managed sync. This is fine (the producer/consumer is the same pass — `vkCmdFillBuffer` followed by a compute write to the same buffer), but it's a small leak through the abstraction.
- **MeshRasterizer texture fallback logic is ad-hoc.** `LoadModel`'s heuristic for picking a non-normal-map texture ([MeshRasterizer.cpp:262-280](src/rendering-techniques/mesh-rasterizer/MeshRasterizer.cpp#L262-L280)) is comment-explained but fragile — substring matching on `_NOR` / `_NORM`. Acceptable for an engine in its current scope; would deserve a real material system if MeshRasterizer matures.

### 2.2 Architecture & design decisions — 9 / 10 (A)

**Strengths**

- **The render graph is a serious, well-designed abstraction.** Compared to typical hobby engines (which inline `vkCmdPipelineBarrier` next to every draw), this engine has a real DAG-driven graph: declaration-order-independent execution, reverse-reachability pruning, sink classification (imported / persistent / explicit), generation-tagged handles, typed `ResourceUsage` for precise barriers, async-compute scheduling with iterative demotion, queue-family ownership transfers with semaphore handoffs, and read-ahead final-layout determination. This is in the same league as architecture papers like Frostbite's FrameGraph or Granite's render-graph.
- **Layering is clean and direction-correct.** `Application → RenderingSystem → Renderer → RenderGraph → VWrap → Vulkan`. Each layer talks down, never up. The `RenderingSystem` extraction (REFACTOR.md §4.3) explicitly turned `Application` from a coordinator into a bootstrapper.
- **Producer / consumer separation is in flight and already partial.** [RenderItem](src/rendering/RenderItem.h) + [RenderScene](src/rendering/RenderScene.h) exist, techniques implement `EmitItems`, and passes can declare `AcceptsItemTypes`. SCENE-GRAPH.md describes the next step — a `Scene` + `SceneExtractor` + `AssetRegistry` — but the contract the future scene graph will plug into is *already in the code*. This is the right way to do a phased refactor.
- **Editor / engine separation is excellent.** The editor never reaches into the graph or VWrap directly. It receives a `GraphSnapshot` (immutable, value-typed) and walks `IInspectable*` for parameter editing. Adding a new panel does not require touching any rendering code.
- **Async compute is genuinely correct, not just labeled.** Cross-stream barriers split into release+acquire halves, semaphore handoff is per-frame-in-flight, conservative consumer-stage wait. Many engines never reach this level of correctness. The intentional decision to skip per-pass profiler timestamps on the async cb (because the query pool is reset on the graphics cb) is noted in a comment ([RenderGraph.cpp:1456-1462](src/rendering/RenderGraph.cpp#L1456-L1462)).
- **Excellent strategic-design documentation.** [REFACTOR.md](REFACTOR.md) and [SCENE-GRAPH.md](SCENE-GRAPH.md) are uncommonly good design documents — diagnostic, prescriptive, dependency-ordered, with effort tags and an explicit "things considered and rejected" section. The actual code is recognizably the result of executing REFACTOR.md.
- **Minimal-virtual interface design.** `RenderTechnique` has just 4 required virtuals (`DescribeTargets`, `RegisterPasses`, `GetShaderPaths`, plus `IInspectable::GetDisplayName`/`GetParameters`). Optional hooks (`OnPostCompile`, `EmitItems`, `Reload`) have empty defaults. Easy to implement a new technique without ceremony.
- **Build system is clean.** CMake fetches GLFW / GLM / spdlog via FetchContent, vendors ImGui / VMA / stb / TinyObjLoader, and shells out to glslc for shader builds. No external installer required beyond the Vulkan SDK.

**Weaknesses**

- **`Application` still touches scene-texture binding lifecycle directly.** `m_editor.RemoveSceneTexture()` / `RegisterSceneTexture(...)` callbacks ([Application.cpp:48-54](src/Application.cpp#L48-L54)) fire around graph rebuilds. This works, but the editor *could* observe its own texture-needs-rebind via the `OnAfterGraphRebuild` callback if `RenderingSystem` exposed a sufficient signal.
- **`RenderingSystem` still has a manual `vkDeviceWaitIdle` strategy** rather than relying on per-resource fence tracking. Acceptable trade-off for now (event-driven rebuilds are infrequent), but it does stall the GPU on every shader hot-reload.
- **The `RenderItem` / `RenderScene` model assumes uniform asset binding per pass.** SCENE-GRAPH.md §4.3 acknowledges this and proposes bindless / per-pass updates / push descriptors as the answer when `InstancedVoxelMesh` lands. Today this is unresolved; the existing techniques each bind one volume / one texture statically.
- **Lighting lives on `Renderer`, not `Scene`.** SCENE-GRAPH.md §6.2 plans to move it. Today `RenderContext::lighting` is wired with a `const_cast` ([RenderingSystem.cpp:64](src/rendering/RenderingSystem.cpp#L64)) — a small cosmetic blemish that the planned move resolves.
- **`PostProcessChain::Register` returns one image handle.** Fine for a linear chain; a real multi-output post-process system (e.g., HDR exposure feeding both bloom and the tonemapper) would need a bigger surface. REFACTOR.md §4.2 explicitly defers this.
- **Subpass merging is not exploited.** Multi-pass post-process effects on a tile-based GPU (mobile Apple Silicon) would benefit; today every pass is its own VkRenderPass with explicit memory barriers. REFACTOR.md flags this as out-of-scope for now.
- **`PipelineLayout` is not yet a separate VWrap class.** REFACTOR.md §3.5 still pending — pipeline layout is buried inside `Pipeline`/`ComputePipeline`. This means two pipelines that *should* share a layout don't share Vulkan-side.

### 2.3 Maintainability — 7 / 10 (B+)

**Strengths**

- **Small, focused files in most places.** Median file is ~100 lines. Most translation units serve one cohesive purpose ([BindingTable](src/rendering/BindingTable.cpp) is 160, [DAGBuilder](src/rendering/DAGBuilder.cpp) is 213, [GraphicsPassBuilder](src/rendering/GraphicsPassBuilder.cpp) is 252).
- **Explicit invariants and rationale comments.** When something subtle is happening, there's almost always a comment explaining the *why* (the cache TTL aside about the `gen` field, the explanation of why declaration order equals topo order in Phase 1, the rationale for skipping profiler timestamps on async).
- **Centralized, generated path config.** `config::SHADER_DIR`, `config::ASSET_DIR` come from a CMake-generated `config.h`. Moving the build directory or the source tree doesn't break anything.
- **Refactor docs as a maintenance asset.** REFACTOR.md and SCENE-GRAPH.md aren't just planning docs — they double as architectural orientation for a new contributor. Anyone reading them learns the engine.
- **Single, RAII-driven shutdown.** `vkDeviceWaitIdle` then let shared_ptrs unwind. No bespoke teardown code per system.
- **Hot-reload works without restart.** F5 recompiles shaders and rebuilds pipelines.

**Weaknesses (this is the lowest of the four scores, here's why)**

- **No automated tests.** It is impossible to tell whether a change to the barrier synthesis or DAG builder regresses correctness without running the whole engine against representative scenes — and no representative scenes are saved. A new hire (or future-you in 6 months) cannot make confident changes to [RenderGraph.cpp](src/rendering/RenderGraph.cpp) ComputeBarriers without manual playtest. This *will* bite as the engine grows.
- **`RenderGraph.cpp` is a single 1556-LOC file.** Even with banner-comment partitioning, IDE navigation (jump-to-definition, fuzzy file open) suffers. A ~3-way split would help.
- **No CI / build verification on PRs.** The repo has CMake but no GitHub Actions workflow visible. Multi-platform (Mac / Linux / Windows) is *claimed* but not *enforced*.
- **No CHANGELOG / migration notes** for breaking changes within the engine API. The `RenderTechnique` interface has changed substantially during the recent refactor; an in-tree document tracking that helps internal stability.
- **Per-technique reload state is fiddly.** `MeshRasterizer` carries `m_pending_model_reload`, `m_pending_texture_reload`, `m_pending_geometry_upload` flags polled in `Reload(ctx)`. REFACTOR.md §2.1 anticipates this pain ("Replace polled `NeedsReload`/`NeedsRebuild` flags with an event channel"). The `AppEvent` system is now in place but techniques haven't fully adopted it for these specific flags.
- **Inspector parameter system is permissive but loosely typed.** `TechniqueParameter::data` is `void*`, dispatched by an enum. Acceptable for an internal tool, but moving to `std::variant<float*, int*, bool*, …>` would shift errors from runtime to compile time.
- **Some stale references in docs.** [README.md](README.md#L97) links to a non-existent ARCHITECTURE.md. Small, but worth fixing once you've decided which living doc plays that role.
- **Selective wrapper coverage in VWrap.** `CommandBuffer` has many but not all `Cmd*` helpers, leading to a sprinkle of raw `vkCmd*` calls in technique code. Not painful — but new contributors have to learn which API to reach for.

### 2.4 Extensibility — 8.5 / 10 (A)

**Strengths**

- **Adding a new render technique is small and prescribed.** Implement 4 virtuals on `RenderTechnique`, declare a binding table, declare passes, implement record callbacks. The current techniques are a working template — copy-and-modify works. `Application::Init` registers techniques in a list; switching is an event.
- **Adding a new post-process effect is even smaller.** Implement `PostProcessEffect`; if it fits "one fragment shader + N inputs + push block" it's ~60 lines via `FullscreenPass::Build` ([BloomEffect.cpp](src/rendering/post-process/BloomEffect.cpp) is 127 lines for *four* passes).
- **Adding a new pass to the graph is one call.** `graph.AddGraphicsPass("name").SetColorAttachment(...).Read(...).SetPipeline(...).SetRecord(...).SetBindings(...);`. No descriptor pool boilerplate, no manual barriers, no manual framebuffer creation, no manual layout transitions.
- **The DAG handles reordering and pruning automatically.** A new pass that doesn't contribute to a sink is dropped without code paths in techniques.
- **`ResourceUsage` enum is open to extension.** Adding (e.g.) `RWStorageImage` or `InputAttachment` is a localized change in the type + the two static `*ReadParams` helpers.
- **Async compute is opt-in per-pass.** A future technique that wants concurrent particle simulation just calls `SetQueueAffinity(QueueAffinity::AsyncCompute)` and the graph does the rest (or transparently demotes if unavailable).
- **`Lifetime` is a clean extension point.** Adding `Lifetime::PoolAliased` (the planned step toward resource aliasing) is a localized change in `AllocateTransientResources` + `Resize`.
- **Inspector accepts any `IInspectable*`.** Lighting, post-process effects, techniques — and (per SCENE-GRAPH.md) future scene nodes and components — all hook in identically.
- **The engine has *already* been extended once on this architecture.** Going from one technique (mesh) to three (+brickmap, +animated geometry) and adding bloom + lens flare did not require touching the graph or `Application`.

**Weaknesses**

- **Per-item resource binding is unresolved.** As SCENE-GRAPH.md §4.3 acknowledges, the static `BindingTable` model breaks for "N items, each with its own asset image." The proposed bindless / texture-array path isn't built yet. This will block `InstancedVoxelMesh` and the foliage feature until designed.
- **Asset loading lives in techniques.** OBJ in `MeshRasterizer`, `.vox` in `BrickmapPaletteRenderer`, palette in both via `PaletteResource`. Adding a fourth technique that reuses any of these means duplicating the loader. SCENE-GRAPH.md §3 plans an `AssetRegistry`; until that lands, asset-shared scenarios pay the duplication tax.
- **Scene state lives on multiple owners.** Camera in `Application`, lighting in `Renderer`, world in techniques. Adding a second camera, a per-scene environment, or a scene-load/save flow forces multi-owner shuffling. SCENE-GRAPH.md addresses this; today it's still distributed.
- **No serialization story.** No scene save/load, no parameter snapshot, no deterministic replay. Acceptable for a renderer toy; non-trivial to add later because nothing was designed with this in mind. SCENE-GRAPH.md §11 explicitly defers serialization.
- **Multi-window / multi-context unsupported.** `Application` is a singleton-shaped class with one `Window`, one `VulkanContext`, one `RenderingSystem`. Adding a second viewport (e.g. Editor + Game) would be invasive.
- **No plug-in / reflection layer.** Adding a new component or asset type is a *code change* — not file-driven. Acceptable for the engine's current scope; SCENE-GRAPH.md explicitly does not recommend ECS or scripting.

---

## Part 3 — Specific recommendations

In rough priority order. None are urgent — the codebase is in good shape — but each meaningfully tightens the engine.

1. **Add unit tests for `DAGBuilder` and `ComputeBarriers`.** These are the two pieces of pure-logic code that *can* regress silently and *will* be invasively edited as features land. Both have plain-data inputs and outputs, which makes them ideal test targets — no Vulkan device required for `DAGBuilder`, and `ComputeBarriers` can run with a mock graph if the resource access path is parameterized. **Highest leverage of any change in this list.**
2. **Split `RenderGraph.cpp` (~1560 LOC) into 3 files.** A natural cut is `RenderGraph.cpp` (resource creation, snapshot, lifecycle), `RenderGraphCompile.cpp` (Compile + DAG + barriers), `RenderGraphExecute.cpp` (RecordStream + Execute + async). The banner comments already mark the boundaries.
3. **Execute the remaining REFACTOR.md items:** §3.5 `PipelineLayout` extraction, §3.4 delete vestigial `RenderPass::Create*` factories. Both are S-effort; both clean up VWrap.
4. **Land SCENE-GRAPH.md step 1–2** (move `SceneLighting` out of `Renderer`; add `AssetRegistry` skeleton). These are the foundation for all the future work and are currently the largest layering smells.
5. **Wire CI.** A GitHub Actions workflow that builds the engine on Linux + macOS + Windows would catch the ~2 platform-specific surprises that always exist.
6. **Add a CHANGELOG / API stability note.** A short doc in the repo root that lists breaking changes to the technique-facing API. Keeps internal stability intact as the engine grows.
7. **Replace polled reload flags with `AppEvent`s.** Per REFACTOR.md §2.1; the queue is now in place.
8. **Standardize naming.** Pick `m_pascalCase` *or* `m_snake_case` and apply uniformly. Mostly a clang-format / sed pass.
9. **Symbolicate magic constants in voxel paths.** `28` for the brick_count offset; the brick size of `8` repeated at multiple call sites; the volume-size header layout. A small `BrickmapLayout` struct or constexpr namespace would centralize and document these.
10. **Fix [README.md](README.md#L97)** to either link to an existing architecture doc or remove the dead reference.

---

## Bottom line

This is an unusually well-architected hobby/portfolio rendering engine. The core abstractions — `RenderGraph`, `BindingTable`, `RenderTechnique`, `RenderItem`/`RenderScene`, `IInspectable` — are right-sized and correctly layered; the graph in particular is a serious, DAG-driven, async-compute-capable design that competes with engines several times its size in concept-count. The recent refactor work (REFACTOR.md, SCENE-GRAPH.md) shows clear, disciplined architectural thinking and the codebase actually reflects it.

The principal weakness is the absence of automated tests against a codebase whose hardest-to-debug surfaces (DAG, barriers, queue handoff) are precisely the kind that *demand* tests. Adding even modest coverage here would meaningfully raise the long-term maintainability ceiling.

The engine is well-positioned to add the next round of features (scene graph, asset registry, instanced foliage, GPU-driven culling) without architectural disruption — the seams those features need are already cut.
