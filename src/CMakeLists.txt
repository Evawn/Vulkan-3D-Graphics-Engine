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
