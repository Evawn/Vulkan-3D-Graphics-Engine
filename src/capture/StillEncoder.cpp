#include "StillEncoder.h"

// stb_image_write was previously instantiated in src/utils/ScreenshotCapture.cpp.
// That file is being deleted as part of this refactor; this is the new TU that
// owns the implementation symbol.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <spdlog/spdlog.h>

bool StillEncoder::Write(const std::filesystem::path& path,
                         uint8_t* rgbaPixels,
                         uint32_t width,
                         uint32_t height) const {
	std::filesystem::create_directories(path.parent_path());
	const int rc = stbi_write_png(path.string().c_str(),
	                              static_cast<int>(width),
	                              static_cast<int>(height),
	                              4,
	                              rgbaPixels,
	                              static_cast<int>(width * 4));
	if (rc) {
		spdlog::get("App")->info("Screenshot saved: {}", path.string());
		return true;
	}
	spdlog::get("App")->error("Failed to write screenshot: {}", path.string());
	return false;
}
