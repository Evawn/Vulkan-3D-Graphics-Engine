file(GLOB SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"
)

# Shared GLSL helpers consumed via #include. Re-trigger compiles when an include changes.
file(GLOB SHADER_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.glsl")

set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

set(SPV_SHADERS "")

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/include" ${SHADER} -o ${SPV_OUTPUT}
        DEPENDS ${SHADER} ${SHADER_INCLUDES}
        COMMENT "Compiling shader: ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND SPV_SHADERS ${SPV_OUTPUT})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_SHADERS})
