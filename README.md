# Vulkan Engine

A C++20 Vulkan rendering engine featuring an ImGui editor interface and multiple rendering techniques switchable at runtime.

![Screenshot](screenshots/screenshot.png)

## Features

- **Multiple rendering techniques** — Octree voxel ray-casting (DDA traversal) and traditional mesh rasterization, switchable at runtime via the editor UI
- **Editor UI** — Dear ImGui docking interface with viewport, metrics, output log, and inspector panels
- **VWrap** — First-party RAII abstraction layer over the Vulkan API (25+ wrapper classes)
- **GPU profiling** — Vulkan timestamp queries, memory statistics, and real-time performance graphs
- **Shader hot-reload** — Recompile and reload shaders without restarting (F5)
- **Cross-platform** — Windows, macOS (via MoltenVK), and Linux
- **MSAA** — Automatic multisampling at device maximum sample count

## Building

### Prerequisites

- [Vulkan SDK](https://vulkan.lunarg.com/) (includes the `glslc` shader compiler)
- [CMake](https://cmake.org/) 3.20+
- A C++20 compatible compiler (GCC 11+, Clang 14+, MSVC 2022+)

### Build

```bash
git clone https://github.com/Evawn/Vulkan-3D-Graphics-Engine.git
cd Vulkan-3D-Graphics-Engine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Then run:

```bash
./build/VulkanEngine        # macOS / Linux
.\build\Release\VulkanEngine.exe   # Windows
```

GLFW, GLM, and spdlog are fetched automatically via CMake FetchContent — no manual dependency setup needed beyond the Vulkan SDK.

## Controls

| Key | Action |
|---|---|
| `W` `A` `S` `D` | Move camera |
| `Space` / `Shift` | Move up / down |
| Mouse | Look around |
| `Escape` | Toggle cursor / UI focus |
| `F5` | Hot-reload shaders |

Click the viewport panel to capture the cursor for camera control. Press Escape to release.

## Project Structure

```
vulkan-engine/
├── src/                 # Application layer
│   ├── rendering/       #   Render techniques (MeshRasterizer, OctreeTracer)
│   ├── editor/          #   ImGui panels and UI rendering
│   └── utils/           #   Logging, GPU profiling, shader compilation
├── lib/VWrap/           # First-party Vulkan RAII wrapper library
├── dep/                 # Vendored third-party headers (ImGui, stb, TinyObjLoader, VMA)
├── shaders/             # GLSL shader source files
├── resources/           # Fonts
└── CMakeLists.txt
```

## Architecture

### VWrap

VWrap is a lightweight RAII wrapper around the Vulkan API. Each Vulkan concept (Instance, Device, Pipeline, Buffer, Image, etc.) is wrapped in a class that manages its own lifetime via `shared_ptr`. Objects are created through static factory methods:

```cpp
auto device = VWrap::Device::Create(physicalDevice, surface);
auto buffer = VWrap::Buffer::Create(allocator, size, usage, memoryFlags);
```

### Render Techniques

Rendering is abstracted behind a `RenderTechnique` interface, allowing different renderers to be added and switched at runtime:

- **OctreeTracer** — Fullscreen ray-casting against a 32x32x32 voxel grid using DDA traversal in a fragment shader
- **MeshRasterizer** — Traditional vertex-based rasterization with OBJ model loading and texturing

### Editor Panels

The editor uses Dear ImGui with docking to provide modular panels:

- **Viewport** — Displays the offscreen-rendered scene
- **Metrics** — FPS, GPU render time, memory stats, performance graphs
- **Inspector** — Renderer selection, parameter tuning, shader reload, screenshot capture
- **Output** — Live log viewer with severity filtering

See [ARCHITECTURE.md](ARCHITECTURE.md) for a deeper dive.

## Dependencies

| Library | Purpose | Source |
|---|---|---|
| [Vulkan SDK](https://vulkan.lunarg.com/) | Graphics API | System install |
| [GLFW](https://www.glfw.org/) | Windowing and input | CMake FetchContent |
| [GLM](https://github.com/g-truc/glm) | Math | CMake FetchContent |
| [spdlog](https://github.com/gabime/spdlog) | Logging | CMake FetchContent |
| [Dear ImGui](https://github.com/ocornut/imgui) | Editor UI | Vendored |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU memory allocation | Vendored |
| [stb_image](https://github.com/nothings/stb) | Image loading | Vendored |
| [TinyObjLoader](https://github.com/tinyobjloader/tinyobjloader) | OBJ mesh loading | Vendored |

## License

This project is licensed under the [MIT License](LICENSE).
