# Architecture Overview

## 1. Project Overview

This is a C++20 Vulkan rendering engine built as a learning project following the [Vulkan Tutorial](https://vulkan-tutorial.com/). The raw tutorial code has been refactored into **VWrap**, a lightweight RAII abstraction layer over the Vulkan API, and an application layer that uses VWrap to implement two rendering paths.

### Current Capabilities

- **Mesh rasterization** — loads OBJ models with textures via TinyObjLoader + stb_image, renders with standard MVP vertex/fragment shaders (currently disabled in the draw loop)
- **Fullscreen ray-casting** — renders a 32x32x32 voxel grid via DDA traversal against a 3D texture, driven entirely from the fragment shader on a fullscreen quad
- **ImGui overlay** — docking-enabled debug UI with FPS counter, render time display, and camera sensitivity/speed sliders
- **GPU profiling** — Vulkan timestamp queries measuring per-frame render time
- **First-person camera** — WASD + mouse look with focus toggling via Escape
- **Frames-in-flight** — double-buffered rendering with per-frame synchronization resources
- **MSAA** — multisampling at the device's maximum supported sample count

### Tech Stack

| Component | Library |
|-----------|---------|
| Graphics API | Vulkan 1.0 |
| Windowing | GLFW 3.3.8 |
| Math | GLM |
| Memory | VMA (Vulkan Memory Allocator) |
| GUI | Dear ImGui (Vulkan + GLFW backends) |
| Image loading | stb_image |
| Model loading | TinyObjLoader |

### State of the Codebase

The project is functional but **Windows-only** (premake5 generating VS2022 solutions, Windows GLFW binaries, .bat shader compile scripts). The Application class is monolithic — it owns every Vulkan object and orchestrates the entire lifecycle. The two renderers (MeshRasterizer, OctreeTracer) share no common interface, making it difficult to swap or compose rendering techniques. VWrap is solid for what it does but lives in `dep/` alongside third-party code, obscuring that it's first-party.

---

## 2. Directory Layout

```
vulkan-engine/
├── premake5.lua                  # Build generator — Windows/VS2022 only (to be replaced)
├── README.md                     # Minimal build instructions
├── .gitignore                    # VS-oriented ignores
├── .gitattributes                # Line ending normalization
│
├── src/                          # Application layer
│   ├── main.cpp                  # Entry point (defines VMA/STB/TINYOBJ _IMPLEMENTATION macros)
│   ├── Application.h/cpp         # Monolithic orchestrator — owns all Vulkan objects, runs main loop
│   ├── MeshRasterizer.h/cpp      # Traditional rasterization renderer (OBJ + texture)
│   ├── OctreeTracer.h/cpp        # Fullscreen ray-casting renderer (3D texture DDA traversal)
│   ├── GUIRenderer.h/cpp         # ImGui wrapper — descriptor pool, draw commands, DPI scaling
│   ├── GPUProfiler.h (.cpp empty) # Vulkan timestamp queries, FPS calculation (all logic in header)
│   ├── Camera.h (.cpp empty)     # FPS camera — view, projection, NDC-to-world matrices (all in header)
│   ├── Input.h/cpp               # Static GLFW input with context-based action binding
│   └── Octree.h/cpp              # Placeholder stub (CreateOctree returns empty data)
│
├── dep/                          # Dependencies (mix of first-party and third-party)
│   ├── VWrap/                    # ** First-party ** Vulkan RAII wrapper library
│   │   ├── include/              #   23 header files
│   │   └── src/                  #   23 source files
│   ├── imgui/                    # Dear ImGui (vendored source + Vulkan/GLFW backends)
│   ├── glm/                      # GLM math library (header-only)
│   ├── Vulkan/Include/           # Vendored Vulkan SDK headers (+ spirv_cross, shaderc, glslang)
│   ├── glfw-3.3.8.bin.WIN64/     # Pre-compiled GLFW Windows binaries
│   ├── stb_image.h               # Single-header image loading
│   ├── tiny_obj_loader.h         # Single-header OBJ model loading
│   └── vk_mem_alloc.h            # Single-header Vulkan Memory Allocator
│
├── shaders/
│   ├── shader_rast.vert/frag     # Rasterization shaders (MVP transform + texture sampling)
│   ├── shader_tracer.vert/frag   # Ray-casting shaders (fullscreen quad + DDA voxel traversal)
│   ├── vert_rast.spv, frag_rast.spv          # Pre-compiled SPIR-V (checked into git)
│   ├── vert_tracer.spv, frag_tracer.spv      # Pre-compiled SPIR-V (checked into git)
│   ├── compile_rast.bat          # Windows shader compile script (hardcoded VulkanSDK path)
│   └── compile_tracer.bat        # Windows shader compile script
│
└── textures/
    └── viking_room.png           # Texture for the mesh rasterizer demo
```

Key observations:
- **VWrap is first-party code living in `dep/`** — it should be separated from third-party dependencies
- **Vendored Vulkan SDK headers and Windows GLFW binaries** — should come from system installs or CMake FetchContent
- **Pre-compiled .spv files in git** — should be build artifacts, not checked in
- **No models/ directory in the repo** — `MeshRasterizer` references `../models/viking_room.obj` which must exist outside the repo

---

## 3. Building & Running on macOS

### Current State (Windows-Only)

The build system uses premake5 to generate Visual Studio 2022 solutions. The `premake5.lua` hardcodes `platforms {"Win64"}`, links against Windows library names (`vulkan-1`, `glfw3`), and references Windows-specific library paths via environment variables. Shader compilation uses `.bat` scripts with a hardcoded `C:\VulkanSDK\` path. None of this works on macOS.

### What Needs to Change

#### 3.1 Build System: premake5 to CMake

Replace `premake5.lua` with a CMake-based build. Recommended structure:

```
CMakeLists.txt              # Root: project setup, C++20, find Vulkan, add subdirectories
├── cmake/
│   └── CompileShaders.cmake   # Custom function for glslc shader compilation
├── dep/
│   └── CMakeLists.txt         # Build VWrap and ImGui as static library targets
├── src/
│   └── CMakeLists.txt         # Application executable target
└── shaders/
    └── CMakeLists.txt         # Shader compilation rules
```

Key CMake details:
- `find_package(Vulkan REQUIRED)` — works cross-platform, provides `Vulkan::Vulkan` imported target and `Vulkan_GLSLC_EXECUTABLE`
- GLFW via `FetchContent` from GitHub (tag 3.3.8) — builds from source, eliminates vendored binaries
- GLM via `FetchContent` — header-only, trivial
- VWrap as `add_library(VWrap STATIC ...)` linked to `Vulkan::Vulkan`
- ImGui as `add_library(ImGui STATIC ...)` with only the needed source files + backends
- Single-header libs (VMA, stb_image, tinyobjloader) stay vendored — they're just headers

#### 3.2 Vulkan SDK & MoltenVK

Install the [LunarG Vulkan SDK for macOS](https://vulkan.lunarg.com/sdk/home) (includes MoltenVK). Then modify VWrap for portability:

**Instance creation** (`dep/VWrap/src/Instance.cpp`):
- Add `VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME` to the instance extensions list
- Set `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` in `VkInstanceCreateInfo::flags`
- Both should be conditional on macOS (`#ifdef __APPLE__` or checking extension availability at runtime)

**Device extensions** (`dep/VWrap/include/Instance.h`):
- Add `"VK_KHR_portability_subset"` to the device extensions list on macOS. MoltenVK devices advertise this extension and Vulkan requires you to enable it if present.

**Queue family selection** (`dep/VWrap/src/PhysicalDevice.cpp`):
- The current `FindQueueFamilies()` searches for a dedicated transfer queue (one with `VK_QUEUE_TRANSFER_BIT` but without `VK_QUEUE_GRAPHICS_BIT`). On MoltenVK, all queues may have both flags. Add a fallback: if no dedicated transfer queue is found, use the graphics queue family for transfers.

**Swapchain format** (`dep/VWrap/src/Swapchain.cpp`):
- Currently prefers `VK_FORMAT_R8G8B8A8_SRGB`. MoltenVK typically offers `VK_FORMAT_B8G8R8A8_SRGB`. Accept both SRGB variants as preferred, or just rely on the existing fallback (`return formats[0]`).

#### 3.3 Shader Compilation

Replace `.bat` scripts with a CMake custom function:

```cmake
function(compile_shader SHADER_SOURCE)
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
    set(SPV_OUTPUT "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${SPV_OUTPUT}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling shader: ${SHADER_NAME}")
endfunction()
```

Remove `.spv` files from git (add `*.spv` to `.gitignore`). Compiled shaders go into the build directory.

#### 3.4 Path Resolution

The codebase uses fragile relative paths (`"../shaders/vert_tracer.spv"`, `"../models/viking_room.obj"`, `"../textures/viking_room.png"`). These break depending on the working directory when launching the executable. Use `std::filesystem` to resolve asset paths relative to the executable location, or define an `ASSET_ROOT` at CMake configure time via a generated header or `#define`.

---

## 4. Architecture Deep Dive

### 4.1 VWrap Abstraction Layer

VWrap wraps 23 Vulkan concepts, each following the same pattern:

```cpp
namespace VWrap {
    class Thing {
    private:
        VkThing m_thing{VK_NULL_HANDLE};        // Raw Vulkan handle
        std::shared_ptr<Device> m_device;         // Parent reference for cleanup
    public:
        static std::shared_ptr<Thing> Create(/*deps*/);  // Factory
        VkThing Get() const { return m_thing; }           // Raw handle access
        ~Thing();                                          // Vulkan cleanup (null-checked)
    };
}
```

**Wrapped concepts** (23 classes):

| Category | Classes |
|----------|---------|
| Core | Instance, PhysicalDevice, Device, Surface, Queue |
| Presentation | Swapchain, FrameController |
| Rendering | RenderPass, Pipeline, Framebuffer |
| Memory | Allocator (VMA), Buffer, Image, ImageView, Sampler |
| Commands | CommandPool, CommandBuffer |
| Descriptors | DescriptorSetLayout, DescriptorPool, DescriptorSet |
| Synchronization | Fence, Semaphore |
| Utilities | Utils (QueueFamilyIndices, SwapchainSupportDetails, Vertex, PushConstantBlock, file I/O, format helpers) |

**Design patterns:**
- **Factory**: All resources created via static `Create()` methods returning `shared_ptr<T>`
- **RAII**: Vulkan handles destroyed in destructors with null-handle guards
- **Shared ownership**: `shared_ptr` forms an implicit dependency graph (Instance -> PhysicalDevice -> Device -> everything else)
- **Facade**: `FrameController` orchestrates Swapchain + CommandBuffers + Semaphores + Fences behind `AcquireNext()` / `Render()` calls
- **Configuration structs**: `PipelineCreateInfo`, `ImageCreateInfo` bundle complex creation parameters

**FrameController** is the most complex VWrap class — it owns the swapchain, per-frame command buffers, semaphores, and fences, and drives the acquire-record-submit-present cycle. It handles swapchain recreation on resize via a callback to the Application.

**Notable details:**
- `Utils.h` is a grab-bag: it contains data structures (Vertex, PushConstantBlock, QueueFamilyIndices), utility functions (readFile, FindDepthFormat), and a Vertex hash function. This should eventually be split.
- `CommandBuffer` has static utility methods for texture uploading and brick texture generation mixed in with command recording — a separation-of-concerns issue.
- `RenderPass::CreateImGUI()` hardcodes a 2-subpass structure (scene + ImGui overlay). This is the main bottleneck for adding multi-pass rendering.

### 4.2 Application Lifecycle

```
main() → Application::Run()
  ├── Init()
  │   ├── InitWindow()      GLFW window, DPI scaling, resize/focus callbacks
  │   ├── InitVulkan()       Instance → Surface → PhysicalDevice → Device → Allocator
  │   │                      → Queues (graphics, present, transfer) → CommandPools
  │   │                      → FrameController (swapchain + sync) → RenderPass
  │   │                      → Color/Depth resources → Framebuffers
  │   ├── InitImGui()        Context, GLFW backend, Vulkan backend (subpass 1)
  │   └── Create renderers   MeshRasterizer, OctreeTracer, GPUProfiler, Camera, Input
  │
  ├── MainLoop()
  │   └── while(!shouldClose):
  │       ├── Calculate delta time
  │       ├── Input::Poll() → ParseInputQuery() → update CameraMoveState
  │       ├── MoveCamera(dt) if focused
  │       ├── GUIRenderer::BeginFrame()
  │       └── DrawFrame()
  │
  └── Cleanup()              ImGui shutdown, GLFW shutdown, shared_ptr destructors handle the rest
```

### 4.3 DrawFrame Pipeline

```
AcquireNext()                           // Wait on fence, acquire swapchain image
Begin command buffer
├── GPUProfiler::CmdBegin()             // Timestamp query (TOP_OF_PIPE)
├── CmdBeginRenderPass (subpass 0)
│   └── OctreeTracer::CmdDraw()         // Bind pipeline, push constants, draw fullscreen quad
├── GPUProfiler::CmdEnd()               // Timestamp query (BOTTOM_OF_PIPE)
├── vkCmdNextSubpass (subpass 1)
│   └── GUIRenderer::CmdDraw()          // ImGui render data
└── vkCmdEndRenderPass
End command buffer
Render()                                // Submit to graphics queue, present
```

The MeshRasterizer's `UpdateUniformBuffer()` and `CmdDraw()` calls are commented out at lines 228-229 of `Application.cpp`. To switch rendering modes, you'd uncomment those and comment out the OctreeTracer call — there's no runtime switching mechanism.

### 4.4 Rendering Paths Compared

| Aspect | MeshRasterizer | OctreeTracer |
|--------|---------------|--------------|
| Topology | Triangle list (indexed) | Triangle strip (4 vertices, no buffer) |
| Vertex data | pos/color/texCoord from OBJ file | None — fullscreen quad generated in vertex shader via `gl_VertexIndex` |
| Uniforms | UBO per frame (model/view/proj matrices) | Push constants (NDCtoWorld mat4 + cameraPos vec3) |
| Descriptors | UBO binding + sampler2D (texture) | sampler3D (32^3 brick texture) |
| Fragment shader | Simple texture lookup | DDA ray-voxel traversal (up to 500 iterations) |
| Depth test | Enabled | Disabled |
| Cull mode | Back face | None |

### 4.5 Ray-Casting Shader

`shader_tracer.frag` implements a DDA (Digital Differential Analyzer) voxel traversal:

1. Reconstruct world-space ray from NDC coordinates using the inverse VP matrix (push constant)
2. Compute ray-AABB intersection against the octree bounds ([-1,1]^3)
3. If hit, step through the 32^3 voxel grid:
   - Fetch voxel value from 3D texture via `texelFetch`
   - If occupied, output hit color (with face-based shading and distance fog)
   - If empty, advance to the next voxel boundary using DDA
   - Track which axis was crossed for face identification
4. If miss, render a sky/horizon gradient based on ray elevation

This is the foundation upon which SVO rendering will be built.

---

## 5. Developer Workflow & QoL Improvements

This section covers the infrastructure that should be in place before building advanced rendering features. A solid development workflow makes iterating on GPU code dramatically faster and less painful.

### 5.1 Build System Migration (premake5 → CMake)

**Why:** premake5 only targets Windows/VS2022. CMake is the industry standard for cross-platform C++ projects, has first-class Vulkan support via `find_package`, and enables CI, dependency management, and shader compilation integration.

**Root CMakeLists.txt** should:
- Set `cmake_minimum_required(VERSION 3.20)`, project name, `CMAKE_CXX_STANDARD 20`
- `find_package(Vulkan REQUIRED)` — provides `Vulkan::Vulkan`, `Vulkan_GLSLC_EXECUTABLE`
- Use `FetchContent` for GLFW (from GitHub, tag 3.3.8) and GLM
- `add_subdirectory(dep)` — builds VWrap and ImGui as static libraries
- `add_subdirectory(src)` — builds the application executable
- `add_subdirectory(shaders)` — shader compilation custom commands

**VWrap target** (`dep/CMakeLists.txt`):
```cmake
add_library(VWrap STATIC
    VWrap/src/Instance.cpp VWrap/src/Device.cpp  # ... all 23 .cpp files
)
target_include_directories(VWrap PUBLIC VWrap/include)
target_link_libraries(VWrap PUBLIC Vulkan::Vulkan)
```

**ImGui target:**
```cmake
add_library(ImGui STATIC
    imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_glfw.cpp imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(ImGui PUBLIC imgui imgui/backends)
target_link_libraries(ImGui PUBLIC Vulkan::Vulkan glfw)
```

**Shader compilation** (`shaders/CMakeLists.txt`):
```cmake
file(GLOB SHADER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.vert" "${CMAKE_CURRENT_SOURCE_DIR}/*.frag")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
    add_custom_command(OUTPUT ${SPV}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER} -o ${SPV}
        DEPENDS ${SHADER}
        COMMENT "Compiling ${SHADER_NAME}")
    list(APPEND SPV_SHADERS ${SPV})
endforeach()
add_custom_target(Shaders ALL DEPENDS ${SPV_SHADERS})
```

After migration, delete `premake5.lua`, the `Solution/` directory, `.bat` shader scripts, `dep/glfw-3.3.8.bin.WIN64/`, and `dep/Vulkan/Include/`. Add `*.spv` to `.gitignore`.

### 5.2 Dependency Management Strategy

| Dependency | Current | Proposed | Rationale |
|-----------|---------|----------|-----------|
| Vulkan SDK | Vendored headers in `dep/Vulkan/` | `find_package(Vulkan)` | System install, cross-platform |
| GLFW | Windows binaries in `dep/` | CMake `FetchContent` | Builds from source on any platform |
| GLM | Vendored in `dep/glm/` | CMake `FetchContent` | Cleaner, always up-to-date |
| ImGui | Vendored source | Keep vendored | No official CMake support, need specific backends |
| VMA | Vendored header | Keep vendored | Single header, stable API |
| stb_image | Vendored header | Keep vendored | Single header |
| TinyObjLoader | Vendored header | Keep vendored | Single header |

The principle: **FetchContent for buildable libraries, keep vendoring single-header libraries**.

### 5.3 Shader Compilation Pipeline

Beyond basic CMake integration:

1. **Remove .spv from git.** Add `*.spv` to `.gitignore`. Shaders compile as a build step.
2. **Include support.** Use `glslc -I shaders/include` to enable `#include` in GLSL. Shared code (common math, struct definitions matching C++ push constants) goes in `shaders/include/`.
3. **SPIRV-Cross reflection (future).** The vendored Vulkan headers already include spirv_cross. Use it to auto-generate descriptor set layouts from compiled SPIR-V, eliminating the manual duplication between GLSL bindings and C++ descriptor setup. This is a significant win when you have many pipelines.

### 5.4 Shader Hot-Reload

This is the single highest-value QoL feature for shader development. When working on ray-casting or compute shaders, being able to tweak code and see results in <1 second is transformative.

**Implementation approach:**
1. Add a keyboard shortcut (e.g., F5) that triggers shader recompilation
2. On trigger: call `glslc` to recompile the .glsl → .spv, then `vkDeviceWaitIdle()`, destroy the old pipeline, create a new one with the updated SPIR-V
3. The current `VWrap::Pipeline::Create()` already creates and immediately destroys shader modules after pipeline creation, so recreating a pipeline is straightforward
4. Optionally, use filesystem watching (`kqueue` on macOS, `inotify` on Linux, `ReadDirectoryChangesW` on Windows) for auto-reload. A lightweight cross-platform library like `efsw` can handle this, but a manual F5 trigger is fine to start.

**Error handling:** If the shader fails to compile, log the `glslc` error output and keep the old pipeline active. Never crash on a shader compilation failure during hot-reload.

### 5.5 Logging Framework

The codebase currently uses `std::cerr` (validation layers), `std::cout` (model loading), `fprintf(stderr, ...)` (VkResult checking), and `throw std::runtime_error` (everywhere). This needs to be unified.

**Recommendation: spdlog** (add via FetchContent)
- Industry-standard, header-only-capable, very fast
- Severity levels: TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL
- Named loggers for subsystems: `[VWrap]`, `[App]`, `[Render]`, `[Input]`, `[GPU]`
- Console + file sinks, with an ImGui sink for the in-engine log viewer (see 5.7)
- Route the Vulkan validation layer callback (`dep/VWrap/src/Instance.cpp` line 38) through spdlog with appropriate severity mapping

Call sites to replace:
- `Instance.cpp:38` — validation layer debug callback → `spdlog::warn/error`
- `Instance.cpp:76-86` — extension enumeration output → `spdlog::debug`
- `Application.h:88` — `fprintf(stderr, ...)` for VkResult → `spdlog::error`
- Model loading `std::cout` lines → `spdlog::info`

### 5.6 Configuration System

Currently hardcoded values are scattered across files:

| Value | Location | Current |
|-------|----------|---------|
| Window size | `Application.h:61-62` | 1200x900 |
| Max frames in flight | `Application.h:67` | 2 |
| Camera FOV, near, far | `Application.cpp:32` | 45, 0.1, 10.0 |
| Camera start position | `Camera.h:33` | (3, 3, 3) |
| Sensitivity, speed | `Application.h:171-172` | 0.5, 5.0 |
| Model path | `MeshRasterizer.h:34` | `../models/viking_room.obj` |
| Texture path | `MeshRasterizer.h:39` | `../textures/viking_room.png` |
| Shader paths | `OctreeTracer.cpp`, `MeshRasterizer.cpp` | `../shaders/*.spv` |

**Recommendation:**
1. Create a `struct EngineConfig` that holds all of these
2. Load from a JSON file using nlohmann/json (single-header, add to dep/) or a TOML parser
3. Resolve asset paths relative to the executable using `std::filesystem`
4. Wire up to ImGui for runtime editing and saving (see 5.7)

### 5.7 ImGui Developer Dashboard

The current `GUIRenderer` is minimal: render time, FPS, a pause button (not wired up), and sensitivity/speed sliders. With docking already enabled (`ImGuiConfigFlags_DockingEnable`), this can expand into a full developer dashboard.

**Recommended architecture:** Create a `DevUI` class that owns registerable panels. Each panel is a small class with a `Draw()` method. `GUIRenderer` stays focused on ImGui/Vulkan integration; `DevUI` handles the content.

**Panels to build:**

1. **Performance** — Frame time graph (`ImGui::PlotLines` over a ring buffer of frame times), GPU render time, CPU frame time, FPS. The existing `GPUProfiler` provides the data.

2. **Camera debug** — Live readout of position, forward, up vectors. FOV/near/far sliders. "Teleport to coordinates" input. Useful for reproducing specific views during debugging.

3. **Log viewer** — Display spdlog output in an ImGui window with severity filtering (checkboxes for TRACE through ERROR). Use a custom spdlog sink that writes to a ring buffer displayed by ImGui.

4. **Renderer selector** — Dropdown to switch active rendering technique at runtime (once the RenderTechnique interface exists, see section 6). Show the current technique's name and any technique-specific parameters.

5. **VMA statistics** — VMA provides `vmaCalculateStatistics()`. Display total allocated memory, allocation count, per-heap breakdown. Essential when working with large voxel datasets.

6. **Config editor** — Expose all `EngineConfig` values as ImGui controls. "Save" button writes to disk. "Reset" restores defaults.

7. **Shader controls** — "Recompile shaders" button (for hot-reload), shader compilation log output, technique-specific shader parameters exposed as uniforms.

### 5.8 Profiling Expansion

The existing `GPUProfiler` records two timestamps per frame (top-of-pipe and bottom-of-pipe). This gives total frame GPU time but no per-pass breakdown.

**Recommended extensions:**

1. **Scoped GPU timers** — Use a larger query pool, pair timestamps around each render pass or technique. Display per-technique timing (e.g., "OctreeTracer: 3.2ms, ImGui: 0.1ms"). When you add shadow passes or post-processing, you'll want to see where time is spent.

2. **CPU frame breakdown** — `std::chrono` timers around input processing, camera update, command buffer recording, queue submission. Display alongside GPU times.

3. **Debug visualizations** — The ray-casting shader already has an iteration counter `i` (line 67 of `shader_tracer.frag`). Add a debug mode that outputs `i / 500.0` as a heatmap instead of the normal color. This shows traversal cost per pixel and is invaluable for optimization. Expose the toggle via ImGui or push constants.

4. **VMA memory tracking** — Log allocation/deallocation events, track peak usage over time.

### 5.9 CI (Continuous Integration)

A GitHub Actions workflow that builds on both platforms catches regressions early:

```yaml
# .github/workflows/build.yml
jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - name: Install Vulkan SDK
        # Use LunarG's official GitHub action or Homebrew
      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build --config Release
```

The shader compilation step validates shaders compile correctly on every push. No runtime/GPU tests needed initially — headless Vulkan testing is complex and not worth the setup for a solo project.

---

## 6. Modular Architecture for Expansion

The long-term goal is pluggable rendering modules: swap in an SVO renderer, a fluid sim, a GI technique, etc. without modifying core engine code. Here's how to evolve from the current monolith.

### 6.1 RenderTechnique Interface

The core abstraction. Both MeshRasterizer and OctreeTracer already share a pattern (Create, CmdDraw, Resize, descriptors, pipeline) but aren't polymorphic. Extract the common interface:

```cpp
class RenderTechnique {
public:
    virtual ~RenderTechnique() = default;

    virtual std::string GetName() const = 0;

    // Lifecycle
    virtual void OnCreate(const RenderContext& ctx) = 0;
    virtual void OnDestroy() = 0;
    virtual void OnResize(VkExtent2D extent) = 0;

    // Per-frame
    virtual void Update(uint32_t frameIndex, const SceneData& scene) = 0;
    virtual void RecordCommands(VkCommandBuffer cmd, uint32_t frameIndex) = 0;
};
```

**RenderContext** bundles everything a technique needs from the engine:

```cpp
struct RenderContext {
    std::shared_ptr<VWrap::Device> device;
    std::shared_ptr<VWrap::Allocator> allocator;
    std::shared_ptr<VWrap::CommandPool> graphicsPool;
    std::shared_ptr<VWrap::RenderPass> renderPass;
    VkExtent2D extent;
    uint32_t maxFramesInFlight;
};
```

The existing renderers become implementations:
- `class MeshRasterizer : public RenderTechnique { ... }`
- `class OctreeTracer : public RenderTechnique { ... }`
- Future: `class SVORenderer : public RenderTechnique { ... }`

The Application's `DrawFrame()` then becomes:

```cpp
activeRenderer->Update(frameIndex, sceneData);
activeRenderer->RecordCommands(cmd, frameIndex);
```

### 6.2 Renderer Registry

A registration-based system for discoverable techniques:

```cpp
class RendererRegistry {
    std::unordered_map<std::string, std::function<std::unique_ptr<RenderTechnique>()>> factories;
public:
    void Register(const std::string& name, auto factory);
    std::unique_ptr<RenderTechnique> Create(const std::string& name);
    std::vector<std::string> GetAvailableRenderers() const;
};
```

Each renderer self-registers. An ImGui dropdown lists available renderers and switches at runtime. Adding a new technique is: implement RenderTechnique, register it, done — no core engine changes needed.

### 6.3 Scene Abstraction

Currently, the Application directly owns a Camera and passes it to renderers. There's no scene representation. A lightweight scene struct keeps the engine generic:

```cpp
struct SceneData {
    Camera camera;

    // Lighting (for rasterization / GI techniques)
    glm::vec3 sunDirection;
    glm::vec3 sunColor;
    float ambientIntensity;

    // Volume data (for voxel / fluid techniques)
    std::shared_ptr<VWrap::Image> volumeTexture;
    glm::mat4 volumeTransform;

    // Mesh data (for rasterization techniques)
    std::vector<MeshInstance> meshes;
};
```

Each RenderTechnique extracts what it needs from SceneData. The Application becomes a thin orchestrator: update SceneData from input, pass it to techniques.

**Not recommended at this stage: full ECS.** Libraries like entt are powerful but designed for game-like scenarios with thousands of entities. A rendering engine focused on technique experimentation doesn't need entity management — it needs clean data flow from scene state to GPU. If game-like features are needed later, an ECS can be introduced then.

### 6.4 Render Graph (Simplified)

The hardcoded 2-subpass RenderPass (`RenderPass::CreateImGUI()`) is the main bottleneck for multi-pass rendering. When you add shadow maps, post-processing, GI accumulation passes, etc., you need dynamic pass management.

A full frame graph (like Frostbite's) is overkill for a solo project. Instead, a simplified render graph:

```cpp
struct RenderPassNode {
    std::string name;
    std::vector<AttachmentDesc> colorAttachments;
    std::optional<AttachmentDesc> depthAttachment;
    std::vector<RenderTechnique*> techniques;    // What renders in this pass
    std::vector<std::string> inputTextures;       // Dependencies on other passes' outputs
};

class RenderGraph {
    std::vector<RenderPassNode> passes;
public:
    void AddPass(RenderPassNode node);
    void Compile();   // Create VkRenderPasses, allocate transient images, topological sort
    void Execute(VkCommandBuffer cmd, uint32_t frameIndex);
};
```

Start simple: a graph with 2 nodes (scene pass, GUI pass) that reproduces the current behavior. Adding a shadow map pass then just means inserting a node before the scene pass. The render graph replaces `RenderPass::CreateImGUI()` with a dynamic builder.

**When to build this:** Not immediately. Build it when you add the first multi-pass technique. Until then, the 2-subpass RenderPass works fine.

### 6.5 Resource Management

Currently, each renderer independently creates its own descriptors, buffers, and images — the setup code in `OctreeTracer::CreateDescriptors()` and `MeshRasterizer::CreateDescriptors()` is structurally identical with different parameters.

A centralized ResourceManager reduces this duplication:

```cpp
class ResourceManager {
    std::shared_ptr<VWrap::Allocator> allocator;
    std::shared_ptr<VWrap::Device> device;
    std::unordered_map<std::string, std::shared_ptr<VWrap::Image>> images;
    std::unordered_map<std::string, std::shared_ptr<VWrap::Buffer>> buffers;
public:
    std::shared_ptr<VWrap::Image> GetOrCreateImage(const std::string& name, const VWrap::ImageCreateInfo& info);
    std::shared_ptr<VWrap::Buffer> GetOrCreateBuffer(const std::string& name, VkDeviceSize size, VkBufferUsageFlags usage);
};
```

Named resources allow sharing between techniques (e.g., a shadow map generated by one pass and sampled by another). This becomes important with the render graph.

### 6.6 Decomposing the Application Monolith

The Application class currently owns: window management, Vulkan context (instance, device, allocator, queues, pools), swapchain/frame management, render pass, framebuffers, all renderers, camera, input, and the main loop. This should be decomposed into:

| Component | Responsibility |
|-----------|---------------|
| `Window` | GLFW creation, callbacks, DPI |
| `VulkanContext` | Instance, PhysicalDevice, Device, Allocator, Queues, CommandPools |
| `FrameController` | Already exists in VWrap — swapchain + sync |
| `RenderGraph` | Pass management, framebuffers |
| `SceneData` | Camera, lights, volumes, meshes |
| `InputSystem` | Input polling, action binding |
| `DevUI` | ImGui panels |
| `Engine` | Thin orchestrator tying the above together |

This isn't a rewrite — it's extracting existing groups of fields and methods into focused classes. The Application already has natural seams (InitWindow, InitVulkan, InitImGui, MainLoop, DrawFrame).

---

## 7. Refactoring Priorities

Ordered by impact and dependency — earlier items unblock later ones.

### Priority 1: CMake + macOS support
**Unblocks:** everything (you can't develop on macOS without this)
**Scope:** New CMakeLists.txt files (root, dep/, src/, shaders/). Delete `premake5.lua`, `.bat` scripts, vendored GLFW Windows binaries, vendored Vulkan headers. Add `*.spv` to `.gitignore`.

### Priority 2: MoltenVK compatibility
**Unblocks:** running on macOS
**Scope:** `Instance.cpp` (portability extensions), `PhysicalDevice.cpp` (queue family fallback), `Swapchain.cpp` (format preference). Small, surgical changes.

### Priority 3: Path resolution
**Unblocks:** running from any working directory, consistent build output
**Scope:** Replace `"../shaders/*.spv"` in OctreeTracer and MeshRasterizer, `"../models/"` and `"../textures/"` in MeshRasterizer. Add std::filesystem-based path resolution or CMake-configured asset root.

### Priority 4: Logging (spdlog)
**Unblocks:** debuggable development, ImGui log viewer
**Scope:** Add spdlog via FetchContent. Replace ~10 output sites across VWrap and app layer. Create named loggers for subsystems.

### Priority 5: RenderTechnique interface
**Unblocks:** runtime renderer switching, all future rendering modules
**Scope:** New `RenderTechnique.h` base class. Refactor `MeshRasterizer` and `OctreeTracer` to implement it. Update `Application::DrawFrame()` to use polymorphic dispatch.

### Priority 6: Decompose Application
**Unblocks:** clean separation of concerns, testable components
**Scope:** Extract `VulkanContext` and `Window` from `Application`. The monolith becomes a thin `Engine` class.

### Priority 7: Move VWrap to lib/
**Unblocks:** clearer project organization
**Scope:** Move `dep/VWrap/` to `lib/VWrap/` (or `engine/VWrap/`). Update CMake paths. This is a 15-minute change but clarifies that VWrap is engine code, not a dependency.

### Priority 8: Shader hot-reload
**Unblocks:** fast shader iteration
**Scope:** Add F5 key binding to recompile shaders and recreate pipelines. Error handling for compilation failures. Optionally add filesystem watching later.

### Priority 9: ImGui dashboard expansion
**Unblocks:** visibility into engine state during development
**Scope:** Create DevUI class with panel registration. Build panels incrementally: performance graph first, then camera debug, then log viewer, then VMA stats. Each panel is ~50-100 lines.

### Priority 10: Render graph
**Unblocks:** multi-pass rendering (shadows, post-processing, GI)
**Scope:** Build when the first multi-pass technique is added. Until then, the current 2-subpass RenderPass works fine.
