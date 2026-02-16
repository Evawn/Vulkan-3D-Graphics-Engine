#include "Application.h"
#include "Log.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#define VMA_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop

int main() {
    Log::Init();

    Application app;

    try {
        app.Run();
    }
    catch (const std::exception& e) {
        spdlog::get("App")->critical("{}", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
