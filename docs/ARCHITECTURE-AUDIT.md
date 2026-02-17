# Architecture Audit

Assessment of the Vulkan engine as a foundation for advanced GPU research — sparse voxel octrees, global illumination, fluid simulation, particle systems, and similar techniques.

**Verdict: Strong foundation with clear extension points.** The render graph and technique plugin system provide a solid base. The main gaps are in resource management (no buffer graph support, no multi-queue async) and missing higher-level systems (scene management, materials) that will become necessary as techniques grow more complex.

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────┐
│  Application          (lifecycle, events, main loop) │
├──────────────┬───────────────────┬───────────────────┤
│  Editor      │  Renderer         │  CameraController │
│  (ImGui UI)  │  (graph builder)  │  (input → camera) │
├──────────────┴───────────────────┴───────────────────┤
│  RenderGraph + PassBuilders                          │
│  (declarative passes, automatic barriers, resources) │
├──────────────────────────────────────────────────────┤
│  RenderTechnique interface                           │
│  ├── DDATracer      (voxel ray marching)             │
│  ├── ComputeTest    (compute → graphics pipeline)    │
│  └── MeshRasterizer (traditional rasterization)      │
├──────────────────────────────────────────────────────┤
│  VWrap  (RAII Vulkan wrappers, VMA allocator)        │
├──────────────────────────────────────────────────────┤
│  Vulkan API                                          │
└──────────────────────────────────────────────────────┘
```

### Module Map

| Directory | Purpose | Key Files |
|-----------|---------|-----------|
| `src/` | App entry, window, camera, input | `Application.cpp`, `Window.cpp`, `Camera.h`, `Input.cpp` |
| `src/rendering/` | Render graph core | `RenderGraph.cpp`, `GraphicsPassBuilder.cpp`, `ComputePassBuilder.cpp` |
| `src/rendering-techniques/` | Pluggable GPU algorithms | `RenderTechnique.h`, `DDATracer.cpp`, `ComputeTest.cpp`, `MeshRasterizer.cpp` |
| `src/editor/` | ImGui editor with dockable panels | `Editor.cpp`, `GUIRenderer.cpp`, panels/ |
| `src/utils/` | Shader compiler, GPU profiler, screenshots | `ShaderCompiler.cpp`, `GPUProfiler.cpp` |

### Frame Flow

```
MainLoop → CameraController::Update → ProcessEvents → DrawFrame
  └→ AcquireNext (fence wait)
  └→ UpdateSwapchainView (per-frame import)
  └→ CommandBuffer::Begin
  └→ GPUProfiler::CmdBegin
  └→ RenderGraph::Execute (barriers → passes → barriers → passes...)
  └→ GPUProfiler::CmdEnd
  └→ CommandBuffer::End
  └→ FrameController::Render (submit + present)
```

---

## Strengths

### 1. Render Graph with Automatic Barrier Management

The declarative render graph (`src/rendering/RenderGraph.cpp`) is the engine's strongest asset. Techniques declare what they need; the graph handles all synchronization.

**What works well:**
- Two-phase model: build (declare resources + passes) then compile (allocate, create render passes, compute barriers)
- Pre-computed barriers avoid per-frame analysis overhead — `ComputeBarriers()` runs once at compile, results stored in `m_barriers` vector indexed by execution step
- Layout tracking per image across all passes with correct `srcStage`/`dstStage` and access mask derivation
- Imported resources (swapchain) with per-frame view updates and framebuffer caching
- Transient MSAA images automatically marked with `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`
- Resize handling that preserves graph structure while reallocating resources

### 2. Technique Plugin System

`RenderTechnique` (`src/rendering-techniques/RenderTechnique.h`) provides a clean interface for adding new rendering algorithms:

```cpp
virtual void RegisterPasses(RenderGraph&, const RenderContext&,
    ImageHandle color, ImageHandle depth, ImageHandle resolve) = 0;
```

The three existing implementations demonstrate the system's range — traditional rasterization, fullscreen ray marching, and a compute-to-graphics pipeline. Adding a new technique requires implementing one class with no changes to engine core.

### 3. First-Class Compute Support

`ComputePassBuilder` has equal standing with `GraphicsPassBuilder`. The `ComputeTest` technique proves the compute → graphics data flow works end-to-end: compute writes to a storage image, the graph automatically transitions it, and a graphics pass samples it. This is the exact pattern needed for GPU simulation techniques.

### 4. Fluent Builder APIs

`GraphicsPassBuilder`, `ComputePassBuilder`, and `DescriptorSetBuilder` all use fluent method chaining that makes pass declaration readable and hard to misconfigure. `PipelineDefaults` provides reusable pipeline state configurations (fullscreen quad, backface culling, depth test variants).

### 5. Editor Integration

The panel-based ImGui editor (`src/editor/`) with dockable viewport, metrics, output log, and inspector provides immediate visual feedback. The `TechniqueParameter` system lets any technique expose tunable parameters to the UI without writing panel code. Hot-reload shaders via `ShaderCompiler` enables rapid iteration.

### 6. Event-Driven State Changes

GPU-mutating operations (shader reload, renderer switch, DPI change) go through an event queue (`Application::ProcessEvents`) that batches device idle waits. This prevents mid-frame Vulkan state corruption and keeps the main loop clean.

---

## Weaknesses

### 1. No Buffer Support in Render Graph

`RenderGraph::CreateBuffer()` is a stub — `handle.id = 0; // TODO` (`src/rendering/RenderGraph.cpp:30`). Buffers are invisible to the graph's barrier computation and lifetime management. Every technique manages its own buffers externally.

**Impact:** Particle systems, fluid simulations, and SVO construction all require SSBO read/write chains across passes. Without graph-tracked buffers, you must manually insert buffer memory barriers inside record callbacks and manage buffer lifetimes yourself. This defeats the purpose of having a render graph.

### 2. Execution Order is Insertion Order

Passes execute in the order `AddGraphicsPass`/`AddComputePass` is called (`m_executionOrder` is a simple append in `RenderGraph.cpp:65,73`). There is no dependency analysis or topological sort based on resource read/write relationships.

**Impact:** Technique authors must manually ensure their passes are registered in the correct order. For simple techniques this works, but multi-pass algorithms (GI with multiple bounces, hierarchical SVO construction) will need careful manual ordering. A dependency-driven sort would make complex pass graphs less error-prone.

### 3. Single Color Attachment Per Graphics Pass

`GraphicsPassBuilder` supports exactly one color attachment, one depth attachment, and one resolve target. There is no MRT (multiple render target) support.

**Impact:** Deferred shading (G-buffer with albedo + normal + depth), screen-space GI, and many post-processing techniques require writing to multiple render targets simultaneously. Currently these would need to be split into multiple passes with redundant geometry processing.

### 4. No Scene or Entity Management

There is no shared scene representation. Each technique is a self-contained island that owns its own geometry, textures, and transforms. The only shared state is the camera (`RenderContext::camera`).

**Impact:** If two techniques need the same mesh data or you want to render a scene with mixed techniques (rasterized geometry + volumetric effects), there's no mechanism for it. Not necessarily a problem for isolated GPU technique experiments, but becomes one if you want techniques to compose over shared scene data.

### 5. Synchronization via vkDeviceWaitIdle

State changes (shader reload, renderer switch, wireframe toggle, viewport resize) all use `vkDeviceWaitIdle` (`Application.cpp:67,157,185`). This is a full GPU pipeline flush.

**Impact:** Acceptable for infrequent operations like shader reloads, but problematic if extended to per-frame operations. For instance, dynamically toggling passes or updating pipeline state per-frame would stall the entire GPU. The render graph's `SetPassEnabled()` method exists but can't be used without a recompile if barriers change.

### 6. Techniques Store Device/Allocator References

Every technique caches `m_device`, `m_allocator`, `m_graphics_pool` etc. from `RenderContext` in `RegisterPasses()` (see `DDATracer.cpp:23-26`, `ComputeTest.cpp:19-21`, `MeshRasterizer` similarly). These are stored as `shared_ptr` member variables.

**Impact:** This is duplicated state — the same pointers exist in `Application`, `Renderer`, `RenderGraph`, and every technique. While `shared_ptr` prevents dangling, it means techniques have broad access to create/destroy GPU resources outside the graph's knowledge. A context-passing pattern (pass `RenderContext` to each virtual method that needs it) would be cleaner.

### 7. No Async Compute / Multi-Queue

All work is submitted on a single graphics queue via one command buffer per frame. There is no overlap between compute and graphics work.

**Impact:** Fluid simulation and particle systems benefit enormously from async compute — running simulation on the compute queue while the previous frame's results are being rendered on the graphics queue. The current architecture would need queue family management, timeline semaphores, and per-queue command buffer recording.

### 8. Static Global Input System

`Input` (`src/Input.h`) is a static class with global state. `Init()` takes a raw `GLFWwindow*` and stores it globally.

**Impact:** Minor for a single-window engine, but makes testing difficult and prevents multiple input contexts. The `Context` abstraction with action bindings is a good start but is underutilized.

### 9. No Resource Aliasing or Lifetime Analysis

The render graph does not analyze resource lifetimes to determine when transient images can share memory. Every `CreateImage` allocates a distinct VMA allocation.

**Impact:** Advanced techniques with many intermediate buffers (mip-chain generation, hierarchical Z, SVO octree levels) waste GPU memory when intermediates could alias. This is an optimization concern rather than a correctness issue — fine for a research engine until memory pressure becomes a problem.

---

## Extensibility Assessment

For each target technique, what's already supported and what gaps need filling.

### Sparse Voxel Octrees

| Aspect | Status |
|--------|--------|
| Compute passes for SVO construction | Supported — `ComputePassBuilder` works |
| SSBO read/write chains between passes | **Blocked** — no buffer graph support |
| 3D texture creation/sampling | Supported — `BrickVolume` + `DDATracer` demonstrate this |
| Push constants for traversal params | Supported — `DDATracer` uses push constants |
| Large buffer allocations (octree nodes) | Manual only — outside graph management |
| Multi-level mip generation | Would need multiple compute passes with manual buffer barriers |

**Bottom line:** The DDATracer is already a starting point. The main gap is buffer graph support for the octree node buffer and brick pool management across construction/traversal passes.

### Global Illumination

| Aspect | Status |
|--------|--------|
| Multi-pass rendering (G-buffer → lighting → composite) | Partially supported — multiple passes work, but no MRT for G-buffer |
| Compute passes for light propagation | Supported |
| Image read/write between passes | Supported — graph handles transitions |
| Temporal accumulation (read previous frame) | Not supported — no persistent frame history mechanism |
| Multiple bounce passes | Supported via multiple compute passes in sequence |
| Deferred shading G-buffer | **Limited** — single color attachment per pass, no MRT |

**Bottom line:** Screen-space techniques (SSAO, SSR) fit well. Deferred shading needs MRT support. Probe-based GI (DDGI) needs 3D texture support (available) plus temporal persistence (not built-in but achievable with imported resources).

### Fluid Simulation

| Aspect | Status |
|--------|--------|
| Compute dispatch chains (pressure solve, advection, etc.) | Supported |
| SSBO ping-pong buffers | **Blocked** — no buffer graph support |
| Async compute for simulation overlap | **Not supported** — single queue |
| 3D texture write from compute | Supported (storage image) |
| Dynamic dispatch sizes | Supported — push constants + manual dispatch |
| Double/triple buffering of simulation state | Manual only — per-frame indexing exists |

**Bottom line:** A basic 2D fluid sim using storage images would work today. 3D fluid simulation with pressure solvers needs buffer graph support and ideally async compute for acceptable frame times.

### Particle Systems

| Aspect | Status |
|--------|--------|
| Compute pass for particle update | Supported |
| SSBO for particle buffer | **Blocked** — no buffer graph support |
| Indirect draw from compute output | Not supported — no indirect draw abstraction |
| Vertex-less rendering (point sprites) | Supported — fullscreen quad pattern generalizes |
| GPU sort (for transparency) | Would need multiple compute passes with buffer ping-pong |
| Emission/death with atomic counters | Manual only — no atomic buffer abstraction |

**Bottom line:** A basic particle system with compute update + point rendering is feasible. Production particle systems with sorting, indirect draw, and variable count need buffer support and indirect draw command recording.

---

## Recommended Improvements

Prioritized by impact on making the engine a better research platform.

### Priority 1: Buffer Support in Render Graph

Add `BufferResource` tracking parallel to `ImageResource`. The graph needs to:
- Track buffer read/write per pass (already stubbed in `PassBuilderBase`)
- Compute buffer memory barriers in `ComputeBarriers()`
- Allocate transient buffers in `AllocateTransientImages()` (rename to `AllocateTransientResources()`)
- Support imported buffers (persistent simulation state)

This unblocks: SVO construction, fluid simulation, particle systems, GPU-driven rendering.

### Priority 2: Multiple Color Attachments (MRT)

Extend `GraphicsPassBuilder` to accept a vector of color attachments instead of a single one. This requires changes to:
- `GraphicsPassBuilder::SetColorAttachment()` → support indexed attachments
- `CreateRenderPass()` → multiple color attachment descriptions
- `CreateFramebuffer()` → multiple attachment views
- `AccumulateUsageFlags()` → iterate all color targets
- `ComputeBarriers()` → track all color targets

This unblocks: deferred shading, G-buffer techniques, any algorithm writing to multiple outputs.

### Priority 3: Dependency-Driven Pass Ordering

Replace insertion-order execution with a topological sort based on resource dependencies. Each pass declares reads/writes; the graph builds a DAG and determines execution order. This makes multi-pass algorithms less fragile and enables future optimizations like pass merging.

### Priority 4: Persistent / Temporal Resources

Add a resource lifetime mode beyond "transient" and "imported":
- **Persistent**: survives across frames, supports double-buffering (read last frame / write current frame)
- Useful for: temporal accumulation (TAA, temporal GI), simulation state (fluid, particles), history buffers

### Priority 5: Indirect Draw and Dispatch

Add helpers for:
- `vkCmdDrawIndirect` / `vkCmdDrawIndirectCount`
- `vkCmdDispatchIndirect`
- Buffer graph integration for indirect argument buffers

This unblocks: GPU-driven rendering, variable-count particle systems, hierarchical culling.

### Lower Priority

- **Async compute**: Multi-queue submission with timeline semaphores. High implementation cost, but significant for overlapping simulation with rendering.
- **Resource aliasing**: Lifetime analysis to share memory between non-overlapping transient resources. Optimization for memory-heavy techniques.
- **Descriptor management overhaul**: Bindless descriptors or descriptor indexing to avoid per-technique descriptor set management. Useful when techniques share resources.
- **Scene abstraction**: Shared geometry/material/transform storage if techniques need to compose over common scene data. Defer this until the need arises — isolated technique experiments don't require it.
