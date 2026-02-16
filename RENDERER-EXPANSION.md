# Renderer Expansion Plan

## Context

Priorities 1-3 and 7 are complete (CMake, MoltenVK, VWrap relocation, path resolution). The engine builds and runs on macOS. This plan implements Priorities 4, 5, 6, 8, and 9 as a unified ImGui docking-based editor workflow, with four panels: a dockable 3D viewport (render-to-texture), performance monitor, log output, and renderer manager.

---

## Phase 1: Logging (spdlog)

No dependencies on other phases. Every subsequent phase uses it.

### 1.1 Add spdlog dependency

- **`CMakeLists.txt`**: Add `FetchContent_Declare(spdlog ...)` with tag `v1.14.1`
- **`lib/CMakeLists.txt`**: Add `spdlog::spdlog` to VWrap's link libraries (for validation layer logging)
- **`src/CMakeLists.txt`**: Add `spdlog::spdlog` to VulkanEngine's link libraries

### 1.2 Create logging infrastructure

- **New: `src/Log.h` / `src/Log.cpp`**
  - `Log::Init()` — creates named loggers: `"VWrap"`, `"App"`, `"Render"`, `"Input"`, `"GPU"`
  - Each logger gets a console color sink + a shared `ImGuiLogSink`
  - Called at top of `main()` before `app.Run()`

- **New: `src/ImGuiLogSink.h`**
  - Custom `spdlog::sinks::base_sink<std::mutex>` subclass
  - Stores `LogEntry { level, logger_name, message }` in a `std::deque` (ring buffer, max 2000)
  - `GetEntries()` / `Clear()` for the Output Panel to consume

### 1.3 Replace output sites

| File | Current | Replacement |
|------|---------|-------------|
| `lib/VWrap/src/Instance.cpp:38` | `std::cerr` (validation callback) | `spdlog::get("VWrap")->warn/error(...)` mapped from Vulkan severity |
| `lib/VWrap/src/Instance.cpp:76-86` | `std::cout` (extensions) | `spdlog::get("VWrap")->debug(...)` |
| `src/Application.h:83-89` | `fprintf(stderr, ...)` | `spdlog::get("App")->error(...)` |
| `src/main.cpp:23` | `std::cerr << e.what()` | `spdlog::get("App")->critical(...)` |
| Various `throw` sites | Throws only | Add `spdlog::error(...)` before each throw |

---

## Phase 2: RenderTechnique Interface

Independent of Phase 1. Foundation for renderer switching, hot-reload, and the manager panel.

### 2.1 Define interface

- **New: `src/RenderTechnique.h`**

```cpp
struct RenderContext {
    std::shared_ptr<VWrap::Device> device;
    std::shared_ptr<VWrap::Allocator> allocator;
    std::shared_ptr<VWrap::CommandPool> graphicsPool;
    std::shared_ptr<VWrap::RenderPass> renderPass;
    VkExtent2D extent;
    uint32_t maxFramesInFlight;
};

class RenderTechnique {
public:
    virtual ~RenderTechnique() = default;
    virtual std::string GetName() const = 0;

    virtual void Init(const RenderContext& ctx) = 0;
    virtual void Shutdown() = 0;
    virtual void OnResize(VkExtent2D newExtent) = 0;

    virtual void RecordCommands(
        std::shared_ptr<VWrap::CommandBuffer> cmd,
        uint32_t frameIndex,
        std::shared_ptr<Camera> camera) = 0;

    // Hot-reload support
    virtual std::vector<std::string> GetShaderPaths() const = 0;
    virtual void RecreatePipeline(const RenderContext& ctx) = 0;
};
```

### 2.2 Refactor existing renderers

- **`src/OctreeTracer.h/cpp`**: `class OctreeTracer : public RenderTechnique`
  - `Create(allocator, device, ...)` -> `Init(RenderContext)` (store ctx internally)
  - `CmdDraw(cmd, frame, camera)` -> `RecordCommands(cmd, frameIndex, camera)` (same signature)
  - `Resize(extent)` -> `OnResize(newExtent)`
  - Add `GetName()` = `"Octree Tracer"`, `GetShaderPaths()`, `RecreatePipeline()` (calls existing `CreatePipeline()`)

- **`src/MeshRasterizer.h/cpp`**: Same pattern
  - Move `UpdateUniformBuffer()` call inside `RecordCommands()` so the interface is uniform
  - `GetName()` = `"Mesh Rasterizer"`

### 2.3 Update Application to use polymorphic dispatch

- **`src/Application.h`**: Replace `std::shared_ptr<OctreeTracer>` with:
  - `std::vector<std::unique_ptr<RenderTechnique>> m_renderers`
  - `size_t m_active_renderer_index = 0`
- **`src/Application.cpp`**: `DrawFrame()` calls `m_renderers[m_active_renderer_index]->RecordCommands(...)`

---

## Phase 3: Application Decomposition + Offscreen Rendering

Depends on Phase 2 (RenderTechnique). This is the largest structural change.

### 3.1 Extract VulkanContext

- **New: `src/VulkanContext.h` / `src/VulkanContext.cpp`**
  - Struct holding: instance, physicalDevice, device, allocator, surface, queues, commandPools, frameController, msaaSamples
  - `VulkanContext::Create(window, enableValidation, maxFramesInFlight)` — moves the body of `Application::InitVulkan()` here
- **`src/Application.h`**: Replace 11 individual Vulkan member variables with `VulkanContext m_vk`

### 3.2 Add new RenderPass factory methods

- **`lib/VWrap/include/RenderPass.h`** — add two new static factories:

  - `CreateOffscreen(device, colorFormat, samples)` — single subpass, 3 attachments (MSAA color + depth + resolve). Resolve attachment `finalLayout = SHADER_READ_ONLY_OPTIMAL` (sampled by ImGui).
  - `CreatePresentation(device, swapchainFormat)` — single subpass, 1 attachment (swapchain image, 1x samples). `finalLayout = PRESENT_SRC_KHR`.

- **`lib/VWrap/src/RenderPass.cpp`** — implement both

### 3.3 Add CmdBeginRenderPass overload

- **`lib/VWrap/include/CommandBuffer.h`** / **`lib/VWrap/src/CommandBuffer.cpp`**
  - Add overload: `CmdBeginRenderPass(renderPass, framebuffer, extent, clearValues)` accepting a `std::vector<VkClearValue>` instead of hardcoding 2 clear values
  - The presentation pass needs only 1 clear value (color), vs the offscreen pass which needs 2 (color + depth)

### 3.4 Create OffscreenTarget

- **New: `src/OffscreenTarget.h` / `src/OffscreenTarget.cpp`**
  - Manages MSAA color image + depth image + resolve image + framebuffer
  - Resolve image has `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`
  - `RegisterWithImGui()` calls `ImGui_ImplVulkan_AddTexture(sampler, resolveView, SHADER_READ_ONLY_OPTIMAL)` returning a `VkDescriptorSet` used as `ImTextureID`
  - `Resize(newExtent)` — removes old ImGui texture, destroys resources (RAII), recreates
  - `GetImGuiTextureID()` — for the viewport panel

### 3.5 Increase ImGui descriptor pool

- **`src/GUIRenderer.cpp`**: Change pool size from `{COMBINED_IMAGE_SAMPLER, 1}` max 1 to `{COMBINED_IMAGE_SAMPLER, 10}` max 10

### 3.6 Restructure DrawFrame

New rendering flow in **`src/Application.cpp`**:

```
AcquireNext()
BeginCommandBuffer

// PASS 1: Scene -> Offscreen
  GPUProfiler::CmdBegin()
  CmdBeginRenderPass(sceneRenderPass, offscreenFramebuffer)
    activeRenderer->RecordCommands(cmd, frameIndex, camera)
  GPUProfiler::CmdEnd()
  vkCmdEndRenderPass()

// PASS 2: ImGui -> Swapchain
  CmdBeginRenderPass(presentationRenderPass, swapchainFramebuffer[imageIndex])
    GUIRenderer::CmdDraw(cmd)   // all panels, including viewport showing offscreen texture
  vkCmdEndRenderPass()

EndCommandBuffer
Render()
```

### 3.7 Update Resize flow

- Presentation framebuffers recreated on swapchain resize (they reference swapchain image views)
- Offscreen target does NOT resize on swapchain resize — it resizes when viewport panel changes size (Phase 5)
- Camera aspect ratio updates from offscreen target extent

### 3.8 Update InitImGui

- `init_info.Subpass = 0` (was 1 — ImGui now renders in presentation pass's only subpass)
- Use `m_presentation_render_pass->Get()` instead of old 2-subpass render pass

---

## Phase 4: Shader Hot-Reload

Depends on Phase 2 (RenderTechnique::RecreatePipeline).

### 4.1 ShaderCompiler utility

- **New: `src/ShaderCompiler.h` / `src/ShaderCompiler.cpp`**
  - `Compile(sourcePath, outputPath)` — runs `glslc` via `popen()`, returns `{success, output, spvPath}`
  - `CompileAll(sourcePaths)` — batch compile
  - Uses `config::GLSLC_PATH` (baked in from CMake)

- **`src/config.h.in`**: Add `GLSLC_PATH = "@Vulkan_GLSLC_EXECUTABLE@"` and `SHADER_SRC_DIR = "@CMAKE_SOURCE_DIR@/shaders"`

### 4.2 Wire into Application

- **`src/Application.h`**: Add `Action::RELOAD_SHADERS` to enum, add F5 binding to `m_main_context`
- **`src/Application.cpp`**: Add `HotReloadShaders()` method:
  1. Map active renderer's SPV paths back to source paths
  2. Compile all via ShaderCompiler
  3. On failure: log errors, keep old pipeline
  4. On success: `vkDeviceWaitIdle()`, call `activeRenderer->RecreatePipeline(ctx)`

---

## Phase 5: ImGui Dashboard with Docking Panels

Depends on all prior phases.

### 5.1 Restructure GUIRenderer

- **`src/GUIRenderer.h/cpp`**:
  - Remove old `CmdDraw(cmd, time, fps, &sensitivity, &speed)` signature
  - New: `CmdDraw(std::shared_ptr<VWrap::CommandBuffer> cmd)` — renders dockspace + all panels
  - Add `RegisterPanel(name, drawFn)` — stores `std::function<void()>` callbacks
  - `CmdDraw` calls `ImGui::DockSpaceOverViewport()` then all registered panel draw functions

### 5.2 Default docking layout (first frame)

Using `ImGui::DockBuilder` API:
```
+---------------------------+---------------+
|                           | Performance   |
|    Renderer View          |---------------|
|    (3D viewport)          | Renderer      |
|                           | Manager       |
+---------------------------+               |
|         Output            |               |
+---------------------------+---------------+
```

### 5.3 Panels

- **New: `src/panels/ViewportPanel.h/cpp`**
  - `Draw()`: `ImGui::Image(offscreenTarget->GetImGuiTextureID(), contentSize)` with zero padding
  - Tracks panel size, signals resize needed via `WasResized()` / `GetDesiredExtent()`
  - Reports `IsFocused()` / `IsHovered()` for camera input gating

- **New: `src/panels/PerformancePanel.h/cpp`**
  - Ring buffer of frame times (120 samples)
  - `ImGui::PlotLines` for frame time + GPU time graphs
  - Numeric readouts: FPS, GPU ms, frame ms

- **New: `src/panels/OutputPanel.h/cpp`**
  - Reads from `ImGuiLogSink::GetEntries()`
  - Severity filter checkboxes (trace/debug/info/warn/error)
  - Color-coded log lines, auto-scroll, clear button

- **New: `src/panels/RendererManagerPanel.h/cpp`**
  - `ImGui::Combo` dropdown listing all `RenderTechnique::GetName()` values
  - On switch: `vkDeviceWaitIdle`, init new technique, update active index
  - "Reload Shaders (F5)" button
  - Displays active technique name and shader paths

### 5.4 Camera input gating

- **`src/Application.cpp MainLoop`**: Camera movement only when `m_app_state.focused && m_viewport_panel.IsHovered()`
- Escape toggles focus as before, but cursor behavior respects viewport hover state

### 5.5 Viewport resize handling

- Each frame, `ViewportPanel::Draw()` checks if panel size changed
- If `WasResized()`, store pending resize. At start of next `DrawFrame()`, before `AcquireNext()`:
  - `vkDeviceWaitIdle()`, `m_offscreen_target->Resize(newExtent)`, update camera aspect ratio, call `activeRenderer->OnResize(newExtent)`

---

## New Files Summary

| File | Purpose |
|------|---------|
| `src/Log.h` / `.cpp` | Named loggers, init, ImGui sink accessor |
| `src/ImGuiLogSink.h` | spdlog sink -> ring buffer for Output Panel |
| `src/RenderTechnique.h` | Abstract interface + RenderContext |
| `src/VulkanContext.h` / `.cpp` | Extracted Vulkan init |
| `src/OffscreenTarget.h` / `.cpp` | Offscreen render target (scene -> ImGui texture) |
| `src/ShaderCompiler.h` / `.cpp` | Runtime glslc invocation |
| `src/panels/ViewportPanel.h` / `.cpp` | 3D viewport as ImGui::Image |
| `src/panels/PerformancePanel.h` / `.cpp` | Frame time graphs, FPS |
| `src/panels/OutputPanel.h` / `.cpp` | Log viewer with filtering |
| `src/panels/RendererManagerPanel.h` / `.cpp` | Technique switching, hot-reload |

## Modified Files Summary

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add spdlog FetchContent |
| `lib/CMakeLists.txt` | Link spdlog |
| `src/CMakeLists.txt` | Link spdlog, add new sources, add panels/ |
| `src/config.h.in` | Add GLSLC_PATH, SHADER_SRC_DIR |
| `lib/VWrap/include/RenderPass.h` | Add CreateOffscreen(), CreatePresentation() |
| `lib/VWrap/src/RenderPass.cpp` | Implement new factories |
| `lib/VWrap/include/CommandBuffer.h` | Add CmdBeginRenderPass overload |
| `lib/VWrap/src/CommandBuffer.cpp` | Implement overload |
| `lib/VWrap/src/Instance.cpp` | Replace std::cerr/cout with spdlog |
| `src/Application.h` | VulkanContext, RenderTechnique vector, panels, new Actions |
| `src/Application.cpp` | Full restructuring: Init, DrawFrame (2-pass), Resize |
| `src/OctreeTracer.h/cpp` | Implement RenderTechnique |
| `src/MeshRasterizer.h/cpp` | Implement RenderTechnique |
| `src/GUIRenderer.h/cpp` | Panel registration, dockspace, larger pool |
| `src/main.cpp` | Add Log::Init() |

## Dependency Graph

```
Phase 1 (Logging) ───────────────────────────> Phase 5 (Panels)
                                                   ^
Phase 2 (RenderTechnique) ──> Phase 4 (Hot-Reload) │
                |                                   │
                └──> Phase 3 (Decompose + Offscreen)┘
```

## Verification

After each phase, the project should build and run:

1. **Phase 1**: Engine runs identically. Logging appears in console (colored, with logger names). Ring buffer accumulates silently.
2. **Phase 2**: Engine runs identically. `m_renderers` vector holds both renderers, polymorphic dispatch works.
3. **Phase 3**: Scene renders to offscreen target. Screen shows only ImGui clear color (dark background) — viewport panel doesn't exist yet. Verify offscreen image is valid by temporarily adding `ImGui::Image()` to GUIRenderer.
4. **Phase 4**: Press F5. Shaders recompile and pipeline recreates. Intentionally break a shader to verify error logging + old pipeline stays active.
5. **Phase 5**: Full docking layout visible. Viewport shows 3D scene. Performance graphs update. Logs stream to output panel. Renderer dropdown switches between OctreeTracer and MeshRasterizer. Shader reload button works. Panels are draggable/resizable.
