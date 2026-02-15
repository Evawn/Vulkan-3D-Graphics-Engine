# Cross-Platform Migration Guide

This guide walks through making the Vulkan engine build and run on Windows, macOS, and Linux. It covers five phases: CMake build system, Vulkan/MoltenVK compatibility, path resolution, dependency management, and CI. Each phase includes exact code, files to modify, gotchas, and verification steps.

The work maps to ARCHITECTURE.md Priorities 1–3, plus GitHub Actions CI.

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Phase 1: CMake Build System Overhaul](#phase-1-cmake-build-system-overhaul)
- [Phase 2: Vulkan / MoltenVK Compatibility](#phase-2-vulkan--moltenvk-compatibility)
- [Phase 3: Path Resolution](#phase-3-path-resolution)
- [Phase 4: Dependency Resolution Strategy](#phase-4-dependency-resolution-strategy)
- [Phase 5: GitHub Actions CI](#phase-5-github-actions-ci)
- [A Note on Docker](#a-note-on-docker)
- [Appendix A: File Change Summary](#appendix-a-file-change-summary)
- [Appendix B: Migration Checklist](#appendix-b-migration-checklist)

---

## Prerequisites

Install these before starting. The Vulkan SDK must be installed system-wide so CMake's `find_package(Vulkan)` can locate it.

| Tool | Windows | macOS | Linux (Ubuntu/Debian) |
|------|---------|-------|-----------------------|
| C++20 compiler | MSVC 2022 (Visual Studio) | Xcode Command Line Tools (`xcode-select --install`) | `sudo apt install build-essential` (GCC 12+) |
| CMake 3.20+ | [cmake.org](https://cmake.org/download/) or `winget install cmake` | `brew install cmake` | `sudo apt install cmake` |
| Vulkan SDK | [LunarG installer](https://vulkan.lunarg.com/sdk/home) | [LunarG macOS installer](https://vulkan.lunarg.com/sdk/home) (includes MoltenVK) | See below |
| Git | [git-scm.com](https://git-scm.com/) | Included with Xcode CLT | `sudo apt install git` |

**Linux Vulkan SDK installation:**

```bash
wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo apt-key add -
sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan.list \
    https://packages.lunarg.com/vulkan/lunarg-vulkan-jammy.list
sudo apt-get update
sudo apt-get install -y vulkan-sdk
```

> **Note:** The PPA URL contains the Ubuntu codename (`jammy` = 22.04, `noble` = 24.04). Match it to your distro version.

**macOS Vulkan SDK setup:**

After installing the LunarG SDK, source the setup script in your shell profile so `find_package(Vulkan)` works:

```bash
# Add to ~/.zshrc or ~/.bash_profile
source ~/VulkanSDK/<version>/setup-env.sh
```

Or set `VULKAN_SDK` manually: `export VULKAN_SDK=~/VulkanSDK/<version>/macOS`

**Minimum versions:** CMake 3.20 (C++20 + FetchContent improvements), Vulkan SDK 1.3.216+ (portability extension support).

---

## Phase 1: CMake Build System Overhaul

This replaces `premake5.lua` (which only generates VS2022 solutions for Win64) with a cross-platform CMake build. After this phase, the project builds on all three platforms from the same configuration.

This phase also moves VWrap out of `dep/` into `lib/`. VWrap is first-party engine code — it should not live alongside third-party dependencies. The `lib/` directory is the standard C++ convention for first-party libraries that are part of the project but logically separate from the application layer.

### 1.1 Target Directory Structure

```
vulkan-engine/
├── CMakeLists.txt              # Root: project setup, find Vulkan, FetchContent, subdirectories
├── lib/
│   ├── CMakeLists.txt          # VWrap static library target
│   └── VWrap/                  # First-party Vulkan RAII wrapper (moved from dep/VWrap/)
│       ├── include/            #   23 header files
│       └── src/                #   22 source files
├── dep/
│   ├── CMakeLists.txt          # ImGui static library target
│   ├── imgui/                  # Dear ImGui (vendored)
│   ├── stb_image.h             # Single-header image loading
│   ├── tiny_obj_loader.h       # Single-header OBJ loading
│   └── vk_mem_alloc.h          # Single-header Vulkan Memory Allocator
├── src/
│   ├── CMakeLists.txt          # Application executable target
│   └── config.h.in             # Path configuration template (generates config.h)
└── shaders/
    └── CMakeLists.txt          # Shader compilation rules
```

The key change from the current layout: `dep/VWrap/` moves to `lib/VWrap/`, and the vendored GLFW binaries (`dep/glfw-3.3.8.bin.WIN64/`), Vulkan headers (`dep/Vulkan/`), and GLM (`dep/glm/`) are deleted entirely — replaced by `find_package(Vulkan)` and `FetchContent`.

### 1.2 Root CMakeLists.txt

Create `CMakeLists.txt` at the project root:

```cmake
cmake_minimum_required(VERSION 3.20)
project(VulkanEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Vulkan SDK (system install) ---
find_package(Vulkan REQUIRED)

# Verify glslc is available (needed for shader compilation)
if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Install the full Vulkan SDK (not just headers).")
endif()

# --- Fetch dependencies ---
include(FetchContent)

# GLFW — builds from source on all platforms
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.8
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# GLM — header-only math library
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 0.9.9.8
)
FetchContent_MakeAvailable(glm)

# --- Subdirectories ---
add_subdirectory(lib)       # VWrap (first-party)
add_subdirectory(dep)       # ImGui and other third-party
add_subdirectory(shaders)   # Shader compilation
add_subdirectory(src)       # Application executable
```

**Considerations:**

- `find_package(Vulkan REQUIRED)` provides the `Vulkan::Vulkan` imported target, `Vulkan_INCLUDE_DIRS`, and `Vulkan_GLSLC_EXECUTABLE`. These work on all platforms when the SDK is installed correctly.
- The `GLFW_BUILD_*` cache variables must be set *before* `FetchContent_MakeAvailable(glfw)` — otherwise GLFW tries to build docs/tests/examples, which may fail on systems missing their dependencies.
- The first `cmake -B build` will be slow as FetchContent clones GLFW and GLM. Subsequent configures use the cached download in `build/_deps/`.

### 1.3 VWrap Static Library — `lib/CMakeLists.txt`

Create `lib/CMakeLists.txt`:

```cmake
# ----- VWrap (first-party Vulkan RAII wrapper) -----
file(GLOB VWRAP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/VWrap/src/*.cpp")

add_library(VWrap STATIC ${VWRAP_SOURCES})

target_include_directories(VWrap PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/VWrap/include"
    "${CMAKE_SOURCE_DIR}/dep"   # For vk_mem_alloc.h, stb_image.h, tiny_obj_loader.h
)

target_link_libraries(VWrap PUBLIC Vulkan::Vulkan glfw glm::glm)
```

**Considerations:**

- VWrap links GLFW because several VWrap headers include `<GLFW/glfw3.h>` (Instance.h, Surface.h, Swapchain.h).
- VWrap links GLM because `Utils.h` includes `<glm/glm.hpp>` and `<glm/gtx/hash.hpp>`.
- `CommandBuffer.h` includes `"stb_image.h"` directly, so `dep/` must be in the include path — that's the `${CMAKE_SOURCE_DIR}/dep` line.
- All VWrap includes in the codebase use bare header names (`#include "Device.h"`, not `#include "VWrap/Device.h"`), so moving VWrap from `dep/VWrap/` to `lib/VWrap/` requires **zero changes to #include statements** — only CMake paths change.

### 1.4 ImGui and Third-Party — `dep/CMakeLists.txt`

Create `dep/CMakeLists.txt`:

```cmake
# ----- Dear ImGui (vendored, specific files only) -----
add_library(ImGui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_vulkan.cpp
)

target_include_directories(ImGui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
)

target_link_libraries(ImGui PUBLIC Vulkan::Vulkan glfw)
```

**Considerations:**

- ImGui is not built via `file(GLOB)` because the `imgui/` directory may contain files we don't want. The explicit file list ensures only the needed sources and Vulkan+GLFW backends are compiled.
- `imgui_demo.cpp` is included because `GUIRenderer.cpp` references `ImGui::ShowDemoWindow()` (even if currently commented out). It compiles to ~200KB — harmless.

### 1.5 Application Executable — `src/CMakeLists.txt`

Create `src/CMakeLists.txt`:

```cmake
file(GLOB APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(VulkanEngine ${APP_SOURCES})

target_link_libraries(VulkanEngine PRIVATE VWrap ImGui glfw glm::glm)

target_include_directories(VulkanEngine PRIVATE
    "${CMAKE_SOURCE_DIR}/dep"
    "${CMAKE_SOURCE_DIR}/lib/VWrap/include"
)

# --- Generate config.h with path aliases ---
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/config.h"
    @ONLY
)
target_include_directories(VulkanEngine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Shaders must be compiled before the executable can run
add_dependencies(VulkanEngine Shaders)
```

**Considerations:**

- `main.cpp` defines `VMA_IMPLEMENTATION`, `STB_IMAGE_IMPLEMENTATION`, and `TINYOBJLOADER_IMPLEMENTATION`. These macros cause the single-header libraries to emit implementation code. Exactly one translation unit can define them — this is already correct and CMake doesn't change it.
- `add_dependencies(VulkanEngine Shaders)` ensures shaders compile before the executable, but doesn't create a link dependency (shaders aren't linked, they're loaded at runtime).
- The `configure_file` call generates `config.h` from `config.h.in` — see Phase 3 for details.

### 1.6 Shader Compilation — `shaders/CMakeLists.txt`

Create `shaders/CMakeLists.txt`:

```cmake
file(GLOB SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
)

set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

set(SPV_SHADERS "")

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER} -o ${SPV_OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader: ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND SPV_SHADERS ${SPV_OUTPUT})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_SHADERS})
```

This replaces both `compile_rast.bat` and `compile_tracer.bat`. It automatically picks up any `.vert` or `.frag` file in `shaders/`.

**Output naming:** `shader_tracer.vert` compiles to `shader_tracer.vert.spv`, `shader_rast.frag` compiles to `shader_rast.frag.spv`. This is simpler and more consistent than the old convention (`vert_tracer.spv`, `frag_rast.spv`). The C++ code paths are updated in Phase 3 to match.

**Considerations:**

- `VERBATIM` is important for cross-platform path handling in `add_custom_command`.
- Shaders compile into the build directory (`build/shaders/`), not the source tree. The runtime path to `.spv` files changes — Phase 3 addresses this.
- The `add_custom_command` tracks the shader source as a dependency, so modifying a `.vert` or `.frag` file triggers recompilation on the next build.
- If `Vulkan_GLSLC_EXECUTABLE` is missing, the root CMakeLists.txt already catches this with a `FATAL_ERROR`.

### 1.7 What to Delete

After CMake is working, remove these files and directories:

| Path | Reason |
|------|--------|
| `premake5.lua` | Replaced by CMakeLists.txt |
| `shaders/compile_rast.bat` | Replaced by shaders/CMakeLists.txt |
| `shaders/compile_tracer.bat` | Replaced by shaders/CMakeLists.txt |
| `shaders/vert_rast.spv` | Now a build artifact (generated in build/shaders/) |
| `shaders/frag_rast.spv` | Now a build artifact |
| `shaders/vert_tracer.spv` | Now a build artifact |
| `shaders/frag_tracer.spv` | Now a build artifact |
| `dep/glfw-3.3.8.bin.WIN64/` | Entire directory — GLFW now fetched via FetchContent |
| `dep/Vulkan/` | Entire directory — Vulkan headers now from system SDK via find_package |
| `dep/glm/` | Entire directory — GLM now fetched via FetchContent |

And **move** (not delete): `dep/VWrap/` → `lib/VWrap/`

### 1.8 .gitignore Updates

Add to `.gitignore`:

```gitignore
# CMake build output
build/
cmake-build-*/
CMakeCache.txt
CMakeFiles/

# FetchContent downloads
_deps/

# Compiled shaders (now build artifacts)
*.spv

# Models directory contents (large binary files)
models/*
!models/.gitkeep
```

The existing `Solution/` ignore (premake output) can be left or removed for cleanliness.

### 1.9 Build Instructions Per Platform

**Windows (MSVC 2022):**

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# Executable: build/src/Release/VulkanEngine.exe
```

**macOS:**

```bash
# Ensure Vulkan SDK env is set (add to ~/.zshrc for persistence):
source ~/VulkanSDK/<version>/setup-env.sh

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Executable: build/src/VulkanEngine
```

**Linux:**

```bash
# Install GLFW build dependencies (needed by FetchContent):
sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Executable: build/src/VulkanEngine
```

> **Note on Linux:** GLFW builds from source via FetchContent and needs windowing system dev headers. Without these packages, CMake configuration will fail with errors about missing X11 or Wayland headers.

### 1.10 Verification

After completing Phase 1:

- [ ] `cmake -B build` completes without errors
- [ ] `cmake --build build` compiles VWrap, ImGui, and the application
- [ ] `.spv` files appear in `build/shaders/`
- [ ] No `.spv` files remain in the `shaders/` source directory
- [ ] The executable runs on Windows (where Vulkan code already works — macOS requires Phase 2)

---

## Phase 2: Vulkan / MoltenVK Compatibility

MoltenVK (the Vulkan-on-Metal translation layer for macOS) requires specific Vulkan extensions and has different hardware characteristics than native Vulkan on Windows/Linux. These are small, surgical changes to VWrap.

### 2.1 Instance Portability Extensions

**File:** `lib/VWrap/src/Instance.cpp`

MoltenVK requires the portability enumeration extension to discover MoltenVK devices. Without it, `vkEnumeratePhysicalDevices` returns zero devices on macOS.

**In `getRequiredExtensions()`** — add the portability extension to the list:

```cpp
// After collecting GLFW extensions and (optionally) debug utils,
// before returning the extensions vector:

#ifdef __APPLE__
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
```

**In `Instance::Create()`** — set the portability enumeration flag on the instance create info:

```cpp
// After setting up createInfo fields, before the vkCreateInstance call:

#ifdef __APPLE__
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
```

**Considerations:**

- The `VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME` macro and `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` flag require Vulkan SDK 1.3.216+. The prerequisites section already specifies this minimum.
- Using `#ifdef __APPLE__` is simpler than runtime detection. For a more robust approach, you could enumerate available instance extensions at runtime and only enable it if present — but the `#ifdef` is sufficient since MoltenVK is macOS-only.

### 2.2 Device Portability Subset Extension

**File:** `lib/VWrap/include/Instance.h`

MoltenVK devices advertise `VK_KHR_portability_subset`, and Vulkan requires you to explicitly enable it. The current `DEVICE_EXTENSIONS` only lists the swapchain extension.

**Modify the `DEVICE_EXTENSIONS` vector:**

```cpp
const std::vector<const char*> DEVICE_EXTENSIONS = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef __APPLE__
    "VK_KHR_portability_subset",
#endif
};
```

**Considerations:**

- The `#ifdef __APPLE__` guard is **mandatory**, not optional. The `checkDeviceExtensions()` function in `PhysicalDevice.cpp` verifies that all required extensions are available. On Windows/Linux, native Vulkan drivers don't advertise `VK_KHR_portability_subset`, so listing it unconditionally would cause physical device selection to fail.
- The string literal `"VK_KHR_portability_subset"` is used instead of a macro because older SDK versions may not provide `VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME`.

### 2.3 Queue Family Selection

**File:** `lib/VWrap/src/PhysicalDevice.cpp`

The `FindQueueFamilies()` function already searches for a dedicated transfer queue — one with `VK_QUEUE_TRANSFER_BIT` but without `VK_QUEUE_GRAPHICS_BIT`. This is the correct behavior: a dedicated transfer queue enables concurrent transfer and graphics operations, which matters for streaming assets during rendering.

The problem is that on MoltenVK, all queue families typically have both flags, so no dedicated transfer family is found. The `isComplete()` check in `Utils.h` requires `transferFamily.has_value()`, causing physical device selection to fail entirely.

**The fix: keep the preference for dedicated queues, add a fallback when none exists.**

After the queue family enumeration loop, before the return statement:

```cpp
if (!indices.transferFamily.has_value()) {
    // No dedicated transfer queue found (common on MoltenVK).
    // Fall back to graphics queue family, which implicitly supports transfers
    // per the Vulkan spec (any GRAPHICS queue also supports TRANSFER).
    // On platforms with dedicated transfer queues (most desktop GPUs on
    // Windows/Linux), the preferred dedicated queue is still used.
    indices.transferFamily = indices.graphicsFamily;
}
```

**Considerations:**

- This preserves the dedicated transfer queue on platforms that support it (most discrete GPUs on Windows/Linux). The fallback only activates when no dedicated queue exists.
- When `transferFamily == graphicsFamily`, `Application.cpp` creates `m_transfer_command_pool` and `m_transfer_queue` using the same family index as graphics. `Device.cpp` uses a `std::set<uint32_t>` of unique queue family indices, so when they're the same index, only one `VkDeviceQueueCreateInfo` is created. This is correct.
- Per the Vulkan spec, any queue family with `VK_QUEUE_GRAPHICS_BIT` implicitly supports `VK_QUEUE_TRANSFER_BIT`. The fallback is always valid.

### 2.4 Swapchain Format Flexibility

**File:** `lib/VWrap/src/Swapchain.cpp`

The `chooseSwapSurfaceFormat()` function currently prefers `VK_FORMAT_R8G8B8A8_SRGB`. MoltenVK typically offers `VK_FORMAT_B8G8R8A8_SRGB` instead. The existing fallback (`return formats[0]`) works but doesn't explicitly prefer an SRGB format.

**Update the format selection to accept both SRGB byte orderings:**

```cpp
VkSurfaceFormatKHR Swapchain::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR> formats)
{
    for (const auto& format : formats) {
        if ((format.format == VK_FORMAT_R8G8B8A8_SRGB ||
             format.format == VK_FORMAT_B8G8R8A8_SRGB) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats[0];
}
```

**Considerations:**

- `R8G8B8A8` and `B8G8R8A8` differ only in byte order. Both are SRGB with the same color quality. The GPU handles the byte swizzle transparently — no shader or framebuffer changes needed.
- This does NOT affect texture formats. `CommandBuffer.cpp` uses `VK_FORMAT_R8G8B8A8_SRGB` for texture image creation — that's the pixel format loaded from disk via stb_image (which always outputs RGBA), and is independent of the swapchain format.

### 2.5 Verification

After completing Phase 2 (requires macOS with Vulkan SDK + MoltenVK installed):

- [ ] Instance creation succeeds (no `VK_ERROR_INCOMPATIBLE_DRIVER`)
- [ ] A physical device is found (no "Failed to find suitable physical device!")
- [ ] The swapchain is created without errors
- [ ] The voxel sphere renders correctly on macOS
- [ ] The ImGui overlay is functional
- [ ] No validation layer errors in the console

---

## Phase 3: Path Resolution

The codebase uses hardcoded relative paths (`"../shaders/vert_tracer.spv"`, `"../models/viking_room.obj"`, etc.) that assume a specific working directory when launching the executable. After the CMake migration, shaders compile into the build directory, and the executable may run from a different location. This phase makes all paths robust.

### 3.1 Current Hardcoded Paths

| File | Line(s) | Current Path | Purpose |
|------|---------|-------------|---------|
| `src/OctreeTracer.cpp` | 45 | `"../shaders/vert_tracer.spv"` | Vertex shader SPIR-V |
| `src/OctreeTracer.cpp` | 46 | `"../shaders/frag_tracer.spv"` | Fragment shader SPIR-V |
| `src/MeshRasterizer.cpp` | 164 | `"../shaders/vert_rast.spv"` | Vertex shader SPIR-V |
| `src/MeshRasterizer.cpp` | 165 | `"../shaders/frag_rast.spv"` | Fragment shader SPIR-V |
| `src/MeshRasterizer.h` | 34 | `"../models/viking_room.obj"` | 3D model file |
| `src/MeshRasterizer.h` | 39 | `"../textures/viking_room.png"` | Texture file |

### 3.2 Strategy: Generated Config Header

Instead of raw `#define` macros, use a CMake-generated config header. This gives type-safe `constexpr` strings with proper namespacing, better IDE support, and no macro pollution.

**Create `src/config.h.in`:**

```cpp
#pragma once

#include <string>

namespace config {
    constexpr const char* SHADER_DIR = "@CMAKE_BINARY_DIR@/shaders";
    constexpr const char* ASSET_DIR  = "@CMAKE_SOURCE_DIR@";
}
```

CMake's `configure_file()` (already added in Phase 1.5) replaces `@CMAKE_BINARY_DIR@` and `@CMAKE_SOURCE_DIR@` with actual paths at configure time, generating `build/src/config.h`.

**Example generated output (macOS):**

```cpp
namespace config {
    constexpr const char* SHADER_DIR = "/Users/you/vulkan-engine/build/shaders";
    constexpr const char* ASSET_DIR  = "/Users/you/vulkan-engine";
}
```

**Why this is better than raw `#define` macros:**

- **Type-safe** — `constexpr const char*` instead of untyped preprocessor text substitution.
- **Scoped** — lives in the `config` namespace, no global macro pollution.
- **IDE-friendly** — IDEs can autocomplete `config::SHADER_DIR`, navigate to the definition, and show the type.
- **Debuggable** — the generated `config.h` is a real file you can inspect in `build/src/config.h`.

**Considerations:**

- This approach embeds absolute paths at compile time. It works perfectly for development. For distribution (shipping the binary to other machines), you'd want `std::filesystem`-based resolution relative to the executable instead — but that's a future concern.
- On Windows, CMake generates forward slashes in paths. `std::ifstream` on Windows accepts forward slashes, so this works as-is.

### 3.3 Updating Call Sites

All files that reference hardcoded paths add `#include "config.h"` and use the `config::` namespace.

**`src/OctreeTracer.cpp`** — lines 45–46:

```cpp
// Before:
auto vert_shader_code = VWrap::readFile("../shaders/vert_tracer.spv");
auto frag_shader_code = VWrap::readFile("../shaders/frag_tracer.spv");

// After:
auto vert_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_tracer.vert.spv");
auto frag_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_tracer.frag.spv");
```

**`src/MeshRasterizer.cpp`** — lines 164–165:

```cpp
// Before:
auto vert_shader_code = VWrap::readFile("../shaders/vert_rast.spv");
auto frag_shader_code = VWrap::readFile("../shaders/frag_rast.spv");

// After:
auto vert_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_rast.vert.spv");
auto frag_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_rast.frag.spv");
```

**`src/MeshRasterizer.h`** — lines 34, 39:

```cpp
// Before:
const std::string MODEL_PATH = "../models/viking_room.obj";
const std::string TEXTURE_PATH = "../textures/viking_room.png";

// After:
const std::string MODEL_PATH = std::string(config::ASSET_DIR) + "/models/viking_room.obj";
const std::string TEXTURE_PATH = std::string(config::ASSET_DIR) + "/textures/viking_room.png";
```

> **Note on shader naming:** The CMake shader compilation (Phase 1.6) outputs `shader_tracer.vert.spv` and `shader_tracer.frag.spv` — the source filename with `.spv` appended. This replaces the old convention of `vert_tracer.spv` and `frag_tracer.spv`. The updated paths above reflect this.

### 3.4 Improved Error Messages

**File:** `lib/VWrap/include/Utils.h` — `readFile()` function (line 99–113)

The current error message (`"failed to open file!"`) doesn't say which file failed. Include the filename:

```cpp
// Before:
throw std::runtime_error("failed to open file!");

// After:
throw std::runtime_error("Failed to open file: " + filename);
```

This makes debugging path issues much easier, especially during the migration when paths are changing.

### 3.5 Models Directory

Create the `models/` directory with a `.gitkeep` file so git tracks the empty directory:

```bash
mkdir -p models
touch models/.gitkeep
```

The `.gitignore` updates from Phase 1.8 already include:

```gitignore
models/*
!models/.gitkeep
```

This means the directory is tracked by git (so cloning the repo creates it), but its contents (large `.obj` files) are not committed. To use the MeshRasterizer, download `viking_room.obj` from the [Vulkan Tutorial resources](https://vulkan-tutorial.com/Loading_models) and place it in `models/`.

### 3.6 Verification

After completing Phase 3:

- [ ] Build the project, then run the executable from the `build/` directory — it should find shaders
- [ ] Run the executable from the project root — it should still find shaders
- [ ] Inspect `build/src/config.h` to verify the generated paths are correct
- [ ] If MeshRasterizer is enabled, it finds `models/viking_room.obj` and `textures/viking_room.png`
- [ ] File-not-found errors now include the full path in the error message

---

## Phase 4: Dependency Resolution Strategy

### 4.1 What Is a Vendored Dependency?

A **vendored dependency** is a library whose source code is copied directly into your repository, rather than being downloaded at build time or installed system-wide. When you vendor a library, its code lives in your repo (typically under `dep/` or `third_party/`), gets committed to git, and is built as part of your project.

**Why not handle all dependencies the same way?** Different libraries have different characteristics that make different strategies optimal:

| Strategy | Best For | Tradeoff |
|----------|----------|----------|
| **System install** (`find_package`) | Large SDKs with platform-specific installers (Vulkan SDK) | Requires manual setup per machine, but provides platform-optimized binaries, tools (glslc), and validation layers |
| **FetchContent** (download at build time) | Multi-file libraries with official CMake support (GLFW, GLM) | First build is slower (cloning), but always builds from source with correct platform flags; keeps repo smaller |
| **Vendored** (copied into repo) | Single-header libraries (VMA, stb_image, tinyobjloader) and libraries without CMake support (ImGui) | Increases repo size, but zero build complexity and works offline with no network required |

The guiding principle: **minimize friction per dependency type.** Single-header libs are one file — there's nothing to "build," so vendoring is trivially simple. Multi-file libs like GLFW have platform-specific compilation that CMake handles automatically via FetchContent. The Vulkan SDK is a full toolchain (headers + libraries + compiler + validation layers) that doesn't make sense to bundle.

### 4.2 Decision Table

| Dependency | Current State | New Source | Rationale |
|-----------|---------------|-----------|-----------|
| **Vulkan SDK** | Vendored headers in `dep/Vulkan/Include/` | `find_package(Vulkan)` — system install | Cross-platform; provides headers, libs, glslc, and validation layers |
| **GLFW 3.3.8** | Pre-compiled Windows binaries in `dep/glfw-3.3.8.bin.WIN64/` | CMake `FetchContent` from GitHub | Builds from source on any platform |
| **GLM** | Full repo vendored in `dep/glm/` | CMake `FetchContent` from GitHub | Removes ~40MB of vendored code; header-only, trivial to fetch |
| **Dear ImGui** | Vendored source in `dep/imgui/` | **Keep vendored** | No official CMake support; we need specific backend files (Vulkan + GLFW) |
| **VMA** | Single header `dep/vk_mem_alloc.h` | **Keep vendored** | Single header, stable API, ~8K lines |
| **stb_image** | Single header `dep/stb_image.h` | **Keep vendored** | Single header, trivially included |
| **tinyobjloader** | Single header `dep/tiny_obj_loader.h` | **Keep vendored** | Single header, trivially included |

### 4.3 Why Dear ImGui Is the Right Choice

ImGui is already integrated into the project and is the right tool for the job. Here's why, and why alternatives don't fit:

**ImGui strengths for this project:**

- **Fully cross-platform** — runs on Windows, macOS, and Linux with any graphics backend. The Vulkan + GLFW backends are first-party, well-maintained, and already in use.
- **De facto standard** for game engine and renderer debug UIs. Used in Unreal Engine, Unity, Godot, and virtually every serious graphics project. This means extensive community knowledge, examples, and tooling.
- **Immediate-mode API** — no widget state management, no layout files, no framework boilerplate. You call `ImGui::SliderFloat("Speed", &speed, 0, 10)` and get a slider. Perfect for debug tools where you want to expose a value quickly.
- **Lightweight** — compiles to ~200KB, no dependencies beyond a graphics backend, no framework lock-in.
- **Docking support** — already enabled in this project (`ImGuiConfigFlags_DockingEnable`). Allows rearrangeable, tabbed panels for building a full developer dashboard.

**Why not alternatives:**

| Library | Why It Doesn't Fit |
|---------|-------------------|
| **Nuklear** | Similar immediate-mode philosophy but less capable. No docking support, smaller ecosystem, fewer backends. |
| **Qt** | Massive framework (~500MB), widget-based (retained mode), designed for desktop applications not game engine overlays. Overkill by orders of magnitude. |
| **egui** | Rust-only. Would require FFI bindings for a C++ project — impractical. |
| **RmlUI** | HTML/CSS-based retained-mode UI. Good for polished game menus, poor fit for rapidly iterating on debug tools. |

### 4.4 Recommended Additional Dependencies

These libraries are not required for the cross-platform migration, but would significantly improve the development experience. All can be added via FetchContent.

#### spdlog — Structured Logging

**What it does:** Provides fast, formatted logging with severity levels (TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL), named loggers for subsystems, and multiple output sinks (console, file, custom).

**Why it helps:** The codebase currently uses a scattered mix of `std::cerr` (validation layers), `std::cout` (model loading), `fprintf(stderr, ...)` (VkResult checking), and `throw std::runtime_error` (everywhere). spdlog unifies all of this behind a single API with filtering and routing. You can route Vulkan validation layer output through `spdlog::warn/error`, add a custom ImGui sink for an in-engine log viewer, and filter by subsystem.

**How to add:**

```cmake
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
FetchContent_MakeAvailable(spdlog)
target_link_libraries(VulkanEngine PRIVATE spdlog::spdlog)
```

#### Tracy — Frame Profiler

**What it does:** A real-time frame profiler with CPU timeline, GPU timeline (Vulkan-native), memory allocation tracking, lock contention visualization, and a standalone viewer application.

**Why it helps:** The current `GPUProfiler` records two timestamps per frame — total GPU time with no per-pass breakdown. Tracy gives you a full timeline view of every CPU function, every GPU pass, every allocation, with nanosecond precision. When you're optimizing ray-casting shaders or adding multi-pass rendering, seeing exactly where time is spent is transformative. It's what RenderDoc is for GPU debugging, but for profiling.

**How to add:**

```cmake
FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.10
)
FetchContent_MakeAvailable(tracy)
target_link_libraries(VulkanEngine PRIVATE TracyClient)
```

Usage is annotation-based: add `ZoneScoped` to functions, `TracyVkZone` around Vulkan command buffer sections. Zero overhead when profiling is disabled.

#### volk — Vulkan Meta-Loader

**What it does:** Loads Vulkan function pointers at runtime instead of linking against the Vulkan loader at build time. A single-header library (but worth mentioning separately from the single-header vendored deps because it changes how Vulkan is linked).

**Why it helps:** Eliminates the link-time dependency on the Vulkan SDK's loader library. The project only needs Vulkan headers at build time — the actual driver is loaded dynamically at runtime. This makes the build more portable (no need to find `libvulkan.so` / `vulkan-1.lib` at link time) and enables loading device-specific function pointers for slightly better call performance.

**How to add:**

```cmake
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 1.3.270
)
FetchContent_MakeAvailable(volk)
target_link_libraries(VWrap PUBLIC volk::volk_headers)
```

Requires calling `volkInitialize()` before any Vulkan calls and `volkLoadDevice(device)` after device creation.

#### nlohmann/json — JSON Parsing

**What it does:** Intuitive JSON parsing and serialization for C++. Header-only, widely used, well-tested.

**Why it helps:** The ARCHITECTURE.md roadmap includes a configuration system (section 5.6) to replace hardcoded values (window size, camera FOV, sensitivity, paths). nlohmann/json makes loading and saving a JSON config file trivial:

```cpp
auto config = nlohmann::json::parse(std::ifstream("engine.json"));
float fov = config["camera"]["fov"].get<float>();
```

**How to add:**

```cmake
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)
target_link_libraries(VulkanEngine PRIVATE nlohmann_json::nlohmann_json)
```

#### fmt — String Formatting

**What it does:** Fast, type-safe string formatting (the library that C++20's `std::format` was based on).

**Why it helps:** Replaces `sprintf`, `std::stringstream`, and ad-hoc string concatenation with a clean, fast API: `fmt::format("Resolution: {}x{}", width, height)`. If your compiler fully supports C++20 `<format>`, you can use `std::format` directly and skip this dependency — but as of 2025, compiler support is still uneven (MSVC is complete, GCC/Clang partial).

**How to add:**

```cmake
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)
FetchContent_MakeAvailable(fmt)
target_link_libraries(VulkanEngine PRIVATE fmt::fmt)
```

---

## Phase 5: GitHub Actions CI

A CI workflow that builds on all three platforms catches cross-platform regressions automatically. No runtime GPU tests — only build and shader compilation validation.

### 5.1 Workflow File

Create `.github/workflows/build.yml`:

```yaml
name: Build

on:
  push:
    branches: [master, dev]
  pull_request:
    branches: [master]

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-latest
            cmake_generator: "Unix Makefiles"
          - os: macos-latest
            cmake_generator: "Unix Makefiles"
          - os: windows-latest
            cmake_generator: "Visual Studio 17 2022"

    runs-on: ${{ matrix.os }}

    steps:
      - uses: actions/checkout@v4

      # --- Linux: Vulkan SDK + GLFW dependencies ---
      - name: Install dependencies (Linux)
        if: runner.os == 'Linux'
        run: |
          wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo apt-key add -
          sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan.list \
              https://packages.lunarg.com/vulkan/lunarg-vulkan-jammy.list
          sudo apt-get update
          sudo apt-get install -y vulkan-sdk
          sudo apt-get install -y libx11-dev libxrandr-dev libxinerama-dev \
              libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev

      # --- macOS: Vulkan SDK via LunarG installer ---
      - name: Install Vulkan SDK (macOS)
        if: runner.os == 'macOS'
        run: |
          brew install molten-vk vulkan-headers vulkan-loader shaderc
          # Set VULKAN_SDK for find_package(Vulkan) — adjust path as needed
          echo "VULKAN_SDK=$(brew --prefix)" >> $GITHUB_ENV

      # --- Windows: Vulkan SDK via community action ---
      - name: Install Vulkan SDK (Windows)
        if: runner.os == 'Windows'
        uses: humbletim/setup-vulkan-sdk@v1.2.0
        with:
          vulkan-query-version: latest
          vulkan-components: Vulkan-Headers, Vulkan-Loader, Glslang, SPIRV-Tools
          vulkan-use-cache: true

      # --- Build ---
      - name: Configure
        run: cmake -B build -G "${{ matrix.cmake_generator }}" -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build --config Release

      # --- Verify shader compilation ---
      - name: Verify shaders compiled
        shell: bash
        run: |
          SPV_COUNT=$(find build/shaders -name "*.spv" | wc -l)
          echo "Found $SPV_COUNT compiled shaders"
          if [ "$SPV_COUNT" -eq 0 ]; then
            echo "ERROR: No .spv files found in build/shaders/"
            exit 1
          fi
```

### 5.2 Platform-Specific Notes

**Linux** — The most straightforward. The LunarG PPA provides everything. GLFW needs windowing dev headers.

**macOS** — The trickiest platform for CI. Options:
- `brew install molten-vk` + related packages: Works but `find_package(Vulkan)` may need `VULKAN_SDK` set manually.
- LunarG installer script: More reliable but requires downloading a ~200MB DMG. The `humbletim/setup-vulkan-sdk` action does not support macOS as of writing.
- If Homebrew's Vulkan packages don't set up `find_package(Vulkan)` correctly, you may need to set `CMAKE_PREFIX_PATH` or `Vulkan_INCLUDE_DIR` / `Vulkan_LIBRARY` manually in the configure step.

**Windows** — The `humbletim/setup-vulkan-sdk` community action handles SDK installation and environment variable setup. The `vulkan-use-cache: true` option caches the SDK between runs for faster CI.

### 5.3 Considerations

- `fail-fast: false` ensures a failure on one platform doesn't cancel the other builds. You want to see all platforms' results.
- There are no runtime tests. Headless Vulkan testing requires either SwiftShader (`VK_GOOGLE_swiftshader`) or Mesa's software Vulkan driver (`mesa-vulkan-drivers`). Both are complex to set up and overkill for a solo project. The CI validates: code compiles, shaders compile, and linking succeeds.
- The macOS Vulkan SDK setup in CI is the most likely pain point. Test it early and iterate. If Homebrew doesn't work, fall back to manually downloading the LunarG installer in the workflow.

### 5.4 Verification

- [ ] Push to the `dev` branch triggers the workflow
- [ ] Linux build passes
- [ ] macOS build passes
- [ ] Windows build passes
- [ ] Shader `.spv` files are verified in the build output

---

## A Note on Docker

Docker is **not included** in this migration plan. Here's why:

**Docker cannot access the GPU on macOS.** Docker Desktop on macOS runs Linux containers inside a lightweight virtual machine (using Apple's Hypervisor.framework). This VM has no GPU passthrough — containers see only a CPU. Since this project requires Vulkan (and on macOS, MoltenVK which needs Metal which needs the GPU), the application simply cannot run inside a Docker container on macOS.

**On Linux with NVIDIA GPUs**, Docker *can* access the GPU via `nvidia-container-toolkit` and `--gpus all`. But this is Linux+NVIDIA only — AMD GPU support is limited, and macOS/Windows are excluded from GPU passthrough entirely.

**Docker for build-only adds no value beyond CI.** If the container can't run the application, its only purpose is validating that the code compiles. GitHub Actions CI (Phase 5) already does this across all three platforms, in a more transparent and maintainable way. Adding a Dockerfile and docker-compose.yml for build-only validation would duplicate the CI setup with extra complexity (Dockerfiles to maintain, X11 dev headers for headless GLFW builds, Vulkan SDK PPA management) and zero additional benefit.

If you later need GPU-accelerated containers (e.g., for headless rendering on a Linux server), revisit Docker at that point with `nvidia-container-toolkit` and the official `nvidia/vulkan` base image.

---

## Appendix A: File Change Summary

| Action | Path | Description |
|--------|------|-------------|
| **CREATE** | `CMakeLists.txt` | Root CMake configuration |
| **CREATE** | `lib/CMakeLists.txt` | VWrap static library target |
| **CREATE** | `dep/CMakeLists.txt` | ImGui static library target |
| **CREATE** | `src/CMakeLists.txt` | Application executable target |
| **CREATE** | `src/config.h.in` | Path configuration template (generates config.h) |
| **CREATE** | `shaders/CMakeLists.txt` | Shader compilation rules |
| **CREATE** | `models/.gitkeep` | Empty directory placeholder |
| **CREATE** | `.github/workflows/build.yml` | CI workflow |
| **MOVE** | `dep/VWrap/` → `lib/VWrap/` | First-party code separated from third-party |
| **MODIFY** | `lib/VWrap/src/Instance.cpp` | Add portability enumeration extension + flag |
| **MODIFY** | `lib/VWrap/include/Instance.h` | Add `VK_KHR_portability_subset` to device extensions |
| **MODIFY** | `lib/VWrap/src/PhysicalDevice.cpp` | Queue family fallback when no dedicated transfer queue |
| **MODIFY** | `lib/VWrap/src/Swapchain.cpp` | Accept both R8G8B8A8 and B8G8R8A8 SRGB formats |
| **MODIFY** | `lib/VWrap/include/Utils.h` | Include filename in `readFile()` error message |
| **MODIFY** | `src/OctreeTracer.cpp` | Use `config::SHADER_DIR` for shader paths |
| **MODIFY** | `src/MeshRasterizer.cpp` | Use `config::SHADER_DIR` for shader paths |
| **MODIFY** | `src/MeshRasterizer.h` | Use `config::ASSET_DIR` for model/texture paths |
| **MODIFY** | `.gitignore` | Add `*.spv`, `build/`, `_deps/`, `models/*` |
| **DELETE** | `premake5.lua` | Replaced by CMake |
| **DELETE** | `shaders/compile_rast.bat` | Replaced by CMake shader compilation |
| **DELETE** | `shaders/compile_tracer.bat` | Replaced by CMake shader compilation |
| **DELETE** | `shaders/*.spv` (4 files) | Now build artifacts |
| **DELETE** | `dep/glfw-3.3.8.bin.WIN64/` | Entire directory — GLFW via FetchContent |
| **DELETE** | `dep/Vulkan/` | Entire directory — Vulkan SDK via system install |
| **DELETE** | `dep/glm/` | Entire directory — GLM via FetchContent |

---

## Appendix B: Migration Checklist

```
Phase 1: CMake Build System
[ ] VWrap moved from dep/VWrap/ to lib/VWrap/
[ ] Root CMakeLists.txt created
[ ] lib/CMakeLists.txt created (VWrap target)
[ ] dep/CMakeLists.txt created (ImGui target)
[ ] src/CMakeLists.txt created (application target)
[ ] src/config.h.in created (path configuration template)
[ ] shaders/CMakeLists.txt created (shader compilation)
[ ] cmake -B build configures without errors
[ ] cmake --build build compiles everything
[ ] .spv files appear in build/shaders/
[ ] Old files deleted (premake5.lua, .bat, vendored GLFW/Vulkan/GLM)
[ ] .gitignore updated
[ ] Builds on Windows
[ ] Builds on macOS
[ ] Builds on Linux

Phase 2: Vulkan / MoltenVK Compatibility
[ ] Instance portability extension added (Instance.cpp)
[ ] Instance portability flag set (Instance.cpp)
[ ] VK_KHR_portability_subset added to device extensions (Instance.h)
[ ] Queue family fallback implemented — prefers dedicated, falls back to graphics (PhysicalDevice.cpp)
[ ] Swapchain accepts both SRGB format variants (Swapchain.cpp)
[ ] Renders correctly on macOS with MoltenVK

Phase 3: Path Resolution
[ ] config.h.in created with SHADER_DIR and ASSET_DIR
[ ] config.h generated correctly in build/src/
[ ] OctreeTracer.cpp shader paths updated to use config::SHADER_DIR
[ ] MeshRasterizer.cpp shader paths updated to use config::SHADER_DIR
[ ] MeshRasterizer.h model/texture paths updated to use config::ASSET_DIR
[ ] readFile() error includes filename (Utils.h)
[ ] models/ directory created with .gitkeep
[ ] Application runs correctly from build directory

Phase 4: Dependencies
[ ] Vulkan SDK via find_package
[ ] GLFW via FetchContent
[ ] GLM via FetchContent
[ ] ImGui builds from vendored source

Phase 5: GitHub Actions CI
[ ] .github/workflows/build.yml created
[ ] Linux build passes
[ ] macOS build passes
[ ] Windows build passes
[ ] Shader compilation verified in CI
```
