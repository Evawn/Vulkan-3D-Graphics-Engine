# ----- VWrap (first-party Vulkan RAII wrapper) -----
file(GLOB VWRAP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/VWrap/src/*.cpp")

add_library(VWrap STATIC ${VWRAP_SOURCES})

target_include_directories(VWrap PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/VWrap/include"
    "${CMAKE_SOURCE_DIR}/dep"   # For vk_mem_alloc.h, stb_image.h, tiny_obj_loader.h
)

target_link_libraries(VWrap PUBLIC Vulkan::Vulkan glfw glm::glm spdlog::spdlog)
