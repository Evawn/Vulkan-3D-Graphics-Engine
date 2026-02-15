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
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# --- Subdirectories ---
add_subdirectory(lib)       # VWrap (first-party)
add_subdirectory(dep)       # ImGui and other third-party
add_subdirectory(shaders)   # Shader compilation
add_subdirectory(src)       # Application executable
