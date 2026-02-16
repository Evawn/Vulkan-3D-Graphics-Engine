# Architecture

## Overview

A Vulkan rendering engine built with C++20. The engine provides a pluggable rendering system, an RAII abstraction layer over Vulkan (VWrap), and an ImGui-based editor with GPU profiling, shader hot-reload, and runtime parameter tuning.

**Tech stack:** Vulkan 1.x, C++20, CMake 3.20+, VMA, Dear ImGui (docking), GLFW, GLM, spdlog

## Project Structure

```
vulkan-engine/
├── src/                           # Application layer
│   ├── main.cpp                   # Entry point
│   ├── Application.h/cpp         # Lifecycle (Init → MainLoop → Cleanup)
│   ├── Camera.h/cpp              # View/projection matrices, NDC-to-world
│   ├── CameraController.h/cpp   # WASD + mouse input
│   ├── Input.h/cpp               # Key state tracking, action mapping
│   ├── rendering/
│   │   ├── RenderTechnique.h     # Abstract base class for renderers
│   │   ├── MeshRasterizer.h/cpp  # Forward rasterization (OBJ + texture)
│   │   └── voxel/
│   │       ├── DDATracer.h/cpp    # Fullscreen DDA voxel ray-casting
│   │       └── BrickVolume.h/cpp  # Voxel data structure
│   ├── editor/
│   │   ├── GUIRenderer.h/cpp     # ImGui context + rendering
│   │   ├── UIStyle.h/cpp         # Theme (dark + green accent)
│   │   ├── ImGuiLogSink.h        # spdlog → ImGui bridge
│   │   └── panels/
│   │       ├── ViewportPanel      # Scene display (offscreen texture)
│   │       ├── MetricsPanel       # FPS, GPU time, memory, graphs
│   │       ├── OutputPanel        # Live log viewer with filtering
│   │       └── InspectorPanel     # Renderer controls, parameters
│   └── utils/
│       ├── Log.h/cpp             # spdlog initialization (6 channels)
│       ├── GPUProfiler.h/cpp     # Vulkan timestamp queries
│       ├── ShaderCompiler.h/cpp  # Runtime glslc compilation
│       └── ScreenshotCapture.h   # PNG export from offscreen target
├── lib/VWrap/                     # First-party Vulkan RAII wrappers
│   ├── include/                   # 25 header files
│   │   ├── VulkanContext.h       # Aggregated context struct
│   │   ├── Instance.h            # VkInstance wrapper
│   │   ├── PhysicalDevice.h      # GPU selection + capability queries
│   │   ├── Device.h              # Logical device
│   │   ├── Allocator.h           # VMA wrapper
│   │   ├── FrameController.h     # Swapchain + frame sync
│   │   ├── CommandBuffer.h       # Recording + one-shot helpers
│   │   ├── Pipeline.h            # Graphics pipeline + layout
│   │   ├── Buffer.h              # Staging/mapped/readback buffers
│   │   ├── Image.h / ImageView.h # Textures and attachments
│   │   ├── OffscreenTarget.h     # MSAA color + depth + resolve
│   │   ├── DescriptorSet/Layout/Pool.h
│   │   ├── RenderPass.h / Framebuffer.h
│   │   ├── Semaphore.h / Fence.h # Sync primitives
│   │   └── Utils.h               # Vertex struct, format helpers
│   └── src/                       # Implementations
├── dep/                           # Vendored third-party
│   ├── imgui/                    # Dear ImGui + Vulkan/GLFW backends
│   ├── vk_mem_alloc.h            # Vulkan Memory Allocator
│   ├── stb_image.h               # Image loading
│   ├── stb_image_write.h         # PNG writing
│   └── tiny_obj_loader.h         # OBJ mesh loading
├── shaders/
│   ├── shader_rast.vert/frag     # Mesh rasterization
│   └── shader_dda.vert/frag      # Voxel ray-casting (DDA)
├── models/                        # OBJ meshes
├── textures/                      # PNG textures
└── resources/fonts/               # ImGui fonts
```

## Core Architecture

### VulkanContext

All core Vulkan objects are aggregated in `VWrap::VulkanContext` and shared via `shared_ptr`:

```
VulkanContext
├── Instance              # Vulkan instance (validation layers in debug)
├── PhysicalDevice        # Selected GPU
├── Device                # Logical device
├── Allocator             # VMA allocator (all GPU memory goes through this)
├── Surface               # Window surface
├── FrameController       # Swapchain + per-frame sync + command buffers
├── graphicsQueue         # Graphics operations
├── presentQueue          # Swapchain presentation
├── transferQueue         # CPU→GPU uploads
├── graphicsCommandPool   # Command buffer allocation (graphics)
├── transferCommandPool   # Command buffer allocation (transfers)
└── msaaSamples           # Max device-supported MSAA level
```

Every VWrap class follows the same pattern: static `Create()` factory method, `shared_ptr` ownership, Vulkan resource cleanup in destructor.

### Application Lifecycle

```
main() → Log::Init() → Application::Run()
                            │
                    ┌───────┴───────┐
                    │     Init()    │
                    │  ┌────────────┤
                    │  │ InitWindow()      — GLFW window (80% screen, centered)
                    │  │ InitVulkan()      — Full Vulkan setup → VulkanContext
                    │  │ InitImGui()       — Dear ImGui with Vulkan backend
                    │  │ Create renderers  — DDATracer + MeshRasterizer
                    │  │ InitPanels()      — Register 4 editor panels
                    │  └────────────┤
                    │   MainLoop()  │ ←── runs until window close
                    │   Cleanup()   │ ←── vkDeviceWaitIdle + shared_ptr teardown
                    └───────────────┘
```

### Frame Pipeline

Each frame records a single command buffer with two render passes:

```
MainLoop()
 │
 ├─ CameraController::Update(dt)
 ├─ HotReloadShaders()           ← if F5 was pressed
 ├─ gui_renderer->BeginFrame()   ← ImGui::NewFrame()
 │
 └─ DrawFrame()
     │
     ├─ FrameController::AcquireNext()
     │   └─ Wait on in-flight fence → acquire swapchain image → reset cmd buffer
     │
     ├─ PASS 1: Scene → Offscreen Target (MSAA)
     │   ├─ BeginRenderPass(scene_render_pass, offscreen_framebuffer)
     │   ├─ GPUProfiler::CmdBegin()           ← timestamp query
     │   ├─ active_renderer->RecordCommands()  ← polymorphic dispatch
     │   ├─ GPUProfiler::CmdEnd()             ← timestamp query
     │   └─ EndRenderPass()
     │
     ├─ PASS 2: UI → Swapchain
     │   ├─ BeginRenderPass(presentation_render_pass, swapchain_framebuffer)
     │   ├─ gui_renderer->CmdDraw()            ← ImGui draws all panels
     │   └─ EndRenderPass()
     │
     └─ FrameController::Render()
         └─ Submit cmd buffer → present swapchain image
            Sync: image_available_sem → render_finished_sem → in_flight_fence
```

Double-buffered: `MAX_FRAMES_IN_FLIGHT = 2`. Each frame has its own command buffer, sync objects, and descriptor sets.

### Memory Management

All GPU allocations go through VMA (`VWrap::Allocator`). The `Buffer` class provides factory methods for common patterns:

| Pattern | Factory Method | Use Case |
|---------|---------------|----------|
| Staging | `Buffer::CreateStaging()` | CPU→GPU transfers (vertices, textures) |
| Persistent-mapped | `Buffer::CreateMapped()` | Per-frame UBO updates (no map/unmap overhead) |
| Device-local | `Buffer::Create(DEVICE_LOCAL)` | Vertex/index buffers, textures |
| Readback | `Buffer::CreateReadback()` | GPU→CPU (screenshots) |

## Rendering Architecture

### RenderTechnique (Strategy Pattern)

New rendering techniques are added by implementing this interface:

```cpp
class RenderTechnique {
    virtual string GetName() const = 0;
    virtual void Init(const RenderContext& ctx) = 0;
    virtual void Shutdown() = 0;
    virtual void OnResize(VkExtent2D newExtent) = 0;
    virtual void RecordCommands(shared_ptr<CommandBuffer> cmd,
                                uint32_t frameIndex,
                                shared_ptr<Camera> camera) = 0;
    virtual vector<string> GetShaderPaths() const = 0;
    virtual void RecreatePipeline(const RenderContext& ctx) = 0;
    virtual vector<TechniqueParameter>& GetParameters();  // UI-exposed params
    virtual FrameStats GetFrameStats() const;
    virtual void SetWireframe(bool enabled);
};
```

`RenderContext` provides the dependencies a technique needs: `Device`, `Allocator`, `CommandPool`, `RenderPass`, `extent`, `maxFramesInFlight`. Techniques are registered in a vector and switchable at runtime via the Inspector panel.

**Current implementations:**
- **MeshRasterizer** — Forward rasterization with OBJ loading (TinyObjLoader), texture sampling (stb_image), per-frame UBO (model/view/proj), wireframe toggle
- **DDATracer** — Fullscreen fragment-shader ray-casting against a 32x32x32 3D voxel texture using DDA traversal, push constants for camera + parameters

### Pipeline

`VWrap::Pipeline` wraps a `VkPipeline` + `VkPipelineLayout`. Currently supports **graphics pipelines only** (vertex + fragment stages). Configuration via `PipelineCreateInfo` struct exposing rasterization, depth/stencil, vertex input, push constants, and descriptor set layout.

### Offscreen Rendering

The scene renders to an `OffscreenTarget` (not directly to the swapchain):

```
OffscreenTarget
├── color_msaa    — MSAA color attachment
├── depth         — MSAA depth attachment
├── resolve       — Non-MSAA resolved image
├── resolve_view  — Sampled by ImGui ViewportPanel
├── sampler       — Linear filtering
└── framebuffer   — Binds all attachments
```

This separation lets ImGui display the scene as a texture within a dockable panel.

## Editor & Dev Tooling

| Feature | Details |
|---------|---------|
| **ImGui Docking UI** | 4 panels (Viewport, Metrics, Output, Inspector), VSCode dark theme |
| **GPU Profiling** | Vulkan timestamp queries, FPS (500ms window), GPU render time (ms), VMA memory stats per heap |
| **Shader Hot-Reload** | F5 triggers `glslc` recompilation → pipeline recreation. Failed compiles keep old pipeline |
| **Logging** | spdlog with 6 channels (App, Render, VWrap, Input, GPU, + ImGui sink). Live filtering in Output panel |
| **Screenshots** | Reads offscreen resolve image → readback buffer → PNG via stb_image_write |
| **Runtime Parameters** | `TechniqueParameter` system: Float/Int/Bool/Color3/Color4/Enum types rendered as sliders/toggles in Inspector |
| **Input System** | Context-based key mapping, cursor capture toggle (Escape), WASD + mouse camera |

## Extensibility Assessment

### Strengths

- **Clean abstraction layer.** VWrap encapsulates Vulkan boilerplate behind RAII classes with `shared_ptr` ownership. Adding a new Vulkan resource type means adding one header + one cpp to `lib/VWrap/`.
- **Pluggable renderers.** The `RenderTechnique` interface makes it straightforward to add new rendering algorithms — implement the interface, push to `m_renderers`, and the UI auto-discovers it.
- **Solid dev tooling baseline.** GPU profiling, hot-reload, structured logging, and runtime parameter tuning are already in place. New techniques get these features for free.
- **Well-separated concerns.** Editor UI, rendering, Vulkan management, and utilities are cleanly separated into distinct directories and responsibilities.

### Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application                              │
│  ┌──────────────┐  ┌─────────────┐  ┌────────────────────────┐ │
│  │ CameraCtrl   │  │ GPUProfiler │  │   GUIRenderer          │ │
│  │ + Input      │  │             │  │  ┌──────┐ ┌─────────┐  │ │
│  └──────┬───────┘  └──────┬──────┘  │  │Viewpt│ │Metrics  │  │ │
│         │                 │         │  ├──────┤ ├─────────┤  │ │
│         ▼                 │         │  │Output│ │Inspector│  │ │
│  ┌──────────────┐         │         │  └──────┘ └─────────┘  │ │
│  │    Camera     │        │         └────────────────────────┘ │
│  └──────┬───────┘         │                                    │
│         │                 │                                    │
│  ┌──────▼─────────────────▼──────────────────────────────────┐ │
│  │              RenderTechnique (active)                      │ │
│  │  ┌────────────────┐  ┌──────────────────┐                 │ │
│  │  │ MeshRasterizer │  │    DDATracer     │  ...extensible  │ │
│  │  └────────────────┘  └──────────────────┘                 │ │
│  └───────────────────────────┬───────────────────────────────┘ │
│                              │                                  │
│  ┌───────────────────────────▼───────────────────────────────┐ │
│  │                      VWrap Layer                           │ │
│  │  Pipeline · DescriptorSet · Buffer · Image · RenderPass   │ │
│  │  CommandBuffer · FrameController · OffscreenTarget        │ │
│  └───────────────────────────┬───────────────────────────────┘ │
│                              │                                  │
│  ┌───────────────────────────▼───────────────────────────────┐ │
│  │                    VulkanContext                            │ │
│  │  Instance · Device · Allocator(VMA) · Queues · Swapchain  │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

## Gap Analysis

The following features are missing for the target use cases (particle/fluid simulations, global illumination, raytracing with acceleration structures):

### Critical

| Gap | What's Missing | Why It Matters |
|-----|---------------|----------------|
| **Compute pipelines** | `Pipeline` only creates graphics pipelines (vertex + fragment). No `VkComputePipeline`, no `vkCmdDispatch`. | Particle simulation, fluid dynamics, prefix sums, histogram generation, and many GI techniques run entirely on compute shaders. This is the single biggest blocker. |
| **Storage buffers (SSBOs)** | `Buffer` doesn't expose `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`. No descriptor support for `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`. | Compute shaders read/write large unstructured data (particle positions, velocity fields, voxel grids) through SSBOs. Uniform buffers are too small (typically 64KB limit). |
| **Raytracing extensions** | No support for `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, or `VK_KHR_ray_query`. No BLAS/TLAS abstractions. | Hardware-accelerated raytracing requires acceleration structure build/update, shader binding tables, and ray tracing pipeline creation — none of which exist in VWrap. |

### High

| Gap | What's Missing | Why It Matters |
|-----|---------------|----------------|
| **Compute queue** | `QueueFamilyIndices` tracks graphics, present, and transfer — but not compute. | Async compute (running particle simulation overlapped with graphics) requires a dedicated compute queue and separate synchronization. |
| **Multi-pass rendering / render graph** | The frame pipeline is hardcoded to 2 passes (scene + UI). No abstraction for pass ordering, dependencies, or resource transitions. | GI techniques (deferred shading, shadow maps, light accumulation, screen-space reflections) require many passes with inter-pass dependencies. A render graph automates resource barriers and pass scheduling. |
| **Scene graph / multi-object rendering** | No scene hierarchy, no transform management, no draw list. Each `RenderTechnique` manages its own single draw. | Rasterization-based GI, shadow mapping, and particle rendering all need to render many objects per frame with different transforms and materials. |
| **Lighting system** | No light objects, no shadow maps, no shading models. MeshRasterizer outputs unlit texture color. | Global illumination, by definition, requires a lighting model to illuminate. PBR materials, point/directional/spot lights, and shadow maps are prerequisites. |
| **Material system** | Single hardcoded texture per renderer. No material abstraction, no PBR parameters. | Different objects need different materials. GI techniques need surface properties (albedo, roughness, metallic, normals) to compute correct light transport. |

### Medium

| Gap | What's Missing | Why It Matters |
|-----|---------------|----------------|
| **Post-processing pipeline** | No tone mapping, bloom, motion blur, or SSAO. The MSAA resolve goes directly to ImGui. | Most advanced rendering techniques output HDR values that need tone mapping. Bloom, SSAO, and motion blur significantly improve visual quality. |
| **Indirect draw** | No `vkCmdDrawIndirect` or `vkCmdDrawIndexedIndirect` support. | GPU-driven rendering (where compute fills a draw-indirect buffer) is essential for rendering millions of particles or procedurally-determined geometry without CPU round-trips. |
| **Timeline semaphores** | Sync uses binary semaphores only. No `VK_SEMAPHORE_TYPE_TIMELINE`. | Fine-grained async compute synchronization (e.g., "wait until compute frame N-1 finishes before graphics frame N reads the buffer") is much cleaner with timeline semaphores than binary semaphores + fences. |
| **Test infrastructure** | No unit tests, no integration tests, no CI/CD. | As the engine grows, untested changes to VWrap or the rendering pipeline can silently break things. Shader compilation tests and VWrap unit tests would catch regressions early. |
| **Dynamic buffer sub-allocation** | No ring buffer or frame-scoped allocator for transient GPU data. | Particle systems and fluid sims update large buffers every frame. A ring buffer avoids per-frame allocation overhead and simplifies multi-frame synchronization. |

## Recommended Roadmap

Priority order for building toward particle/fluid sims, GI, and raytracing:

### Phase 1: Compute Foundation
1. **Add compute pipeline support to VWrap** — `ComputePipeline` class wrapping `VkComputePipeline`, `vkCmdDispatch`, compute shader modules
2. **Add SSBO support** — `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` in `Buffer`, `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` in descriptor abstractions
3. **Add compute queue** — Extend `QueueFamilyIndices` with `computeFamily`, create compute `CommandPool`
4. **First milestone:** Simple GPU particle system as a new `RenderTechnique` — compute shader updates positions, graphics shader renders points/quads

### Phase 2: Multi-Pass & Scene Infrastructure
5. **Multi-object rendering** — Draw list or simple scene graph with per-object transforms and material bindings
6. **Multi-pass support** — Either a lightweight pass scheduler or a render graph that manages resource transitions and pass ordering
7. **Lighting & materials** — Light objects, PBR material struct, shadow map pass
8. **Post-processing** — Tone mapping + bloom as a post-process pass on the offscreen target

### Phase 3: Advanced Techniques
9. **Raytracing extensions** — BLAS/TLAS wrappers, ray tracing pipeline, shader binding table management
10. **Indirect draw** — `vkCmdDrawIndirect` for GPU-driven particle/mesh rendering
11. **Timeline semaphores** — Replace binary semaphores for async compute overlap
12. **Fluid simulation** — SPH or grid-based fluid sim using compute + indirect draw
